#include "controllers/EnrichController.h"
#include "services/AddressParser.h"
#include "services/PreProcessor.h"
#include "services/ConfidenceScorer.h"
#include "services/RuleEngine.h"
#include "services/CacheManager.h"
#include "services/LLMFallback.h"
#include "services/MetricsCollector.h"
#include "models/AddressModels.h"
#include "utils/InputValidator.h"

#include <fstream>
#include <sstream>
#include <algorithm>
#include <unordered_set>

extern addr::AddressParser     g_parser;
extern addr::PreProcessor      g_preprocessor;
extern addr::ConfidenceScorer  g_scorer;
extern addr::RuleEngine        g_rules;
extern addr::CacheManager*     g_cache;
extern addr::LLMFallback       g_llm;
extern addr::ServiceConfig     g_config;

namespace addr {

// =============================================================================
//  Known address column names — checked in priority order
// =============================================================================
static const std::vector<std::string> kAddrCandidates = {
    "addr", "address", "full_address", "location"
};

// Known ID/key column names
static const std::vector<std::string> kKeyCandidates = {
    "id", "uid", "uuid", "key", "pk", "record_id", "row_id", "_id"
};

// =============================================================================
//  detectAddressColumn
//  1. Check kAddrCandidates (case-insensitive)
//  2. Fall back to the member with the longest string value in first_record
// =============================================================================
std::string EnrichController::detectAddressColumn(const Json::Value& rec) {
    if (!rec.isObject()) return "";

    // Step 1: well-known names
    for (const auto& cand : kAddrCandidates) {
        if (rec.isMember(cand) && rec[cand].isString()) return cand;
        // case-insensitive pass
        for (const auto& key : rec.getMemberNames()) {
            std::string lower = key;
            std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
            if (lower == cand && rec[key].isString()) return key;
        }
    }

    // Step 2: longest string field
    std::string best_key;
    size_t best_len = 0;
    for (const auto& key : rec.getMemberNames()) {
        if (rec[key].isString()) {
            size_t len = rec[key].asString().size();
            if (len > best_len) { best_len = len; best_key = key; }
        }
    }
    return best_key;
}

// =============================================================================
//  detectKeyColumn
//  1. Check kKeyCandidates
//  2. Fall back to first integer-valued field
//  3. Fall back to first field overall (skip addr_col)
// =============================================================================
std::string EnrichController::detectKeyColumn(const Json::Value& rec,
                                               const std::string& addr_col) {
    if (!rec.isObject()) return "";

    for (const auto& cand : kKeyCandidates) {
        if (rec.isMember(cand) && cand != addr_col) return cand;
        for (const auto& key : rec.getMemberNames()) {
            std::string lower = key;
            std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
            if (lower == cand && key != addr_col) return key;
        }
    }

    // First integer-valued field
    for (const auto& key : rec.getMemberNames()) {
        if (key != addr_col && (rec[key].isInt() || rec[key].isUInt())) return key;
    }

    // First field that isn't the address column
    for (const auto& key : rec.getMemberNames()) {
        if (key != addr_col) return key;
    }

    return "";
}

// =============================================================================
//  parseNormalizeColumns
//  "ALL" or ""  → empty set (means return all parsed fields)
//  "city,state" → {"city","state"}
// =============================================================================
std::unordered_set<std::string>
EnrichController::parseNormalizeColumns(const std::string& spec) {
    std::unordered_set<std::string> cols;
    if (spec.empty() || spec == "ALL") return cols; // empty = all

    std::istringstream ss(spec);
    std::string token;
    while (std::getline(ss, token, ',')) {
        // trim whitespace
        token.erase(0, token.find_first_not_of(" \t"));
        token.erase(token.find_last_not_of(" \t") + 1);
        if (!token.empty()) cols.insert(token);
    }
    return cols;
}

// =============================================================================
//  buildOutputRecord
//  Merges raw passthrough fields + parsed components + expansions.
//
//  normalized_<addr_col> empty  → include ALL parsed fields
//  normalized_<addr_col> non-empty → include only those parsed fields
//
//  Original fields are ALWAYS passed through unchanged.
// =============================================================================
// =============================================================================
//  buildNormalizedField
//  Constructs a single clean string from parsed components in canonical order.
//  Order: house_number road unit level staircase entrance po_box
//         suburb city_district city state postcode country
//  Rules: lowercase, no punctuation, no double spaces, no leading/trailing spaces.
// =============================================================================
static std::string buildNormalizedField(const ParsedAddress& p) {
    // Canonical field order as per spec
    const std::string* fields[] = {
        &p.house_number, &p.road,     &p.unit,    &p.level,
        &p.staircase,    &p.entrance, &p.po_box,
        &p.suburb,       &p.city_district, &p.city,
        &p.state,        &p.postcode, &p.country
    };

    std::string out;
    out.reserve(128);

    for (const std::string* f : fields) {
        if (f->empty()) continue;
        if (!out.empty()) out += ' ';
        // lowercase + strip punctuation in one pass
        for (unsigned char c : *f) {
            if (std::isalnum(c) || c == ' ') {
                out += static_cast<char>(std::tolower(c));
            } else if (c >= 0x80) {
                out += static_cast<char>(c); // UTF-8 passthrough
            }
            // punctuation silently dropped
        }
    }

    // Collapse any double spaces (can arise from punct stripping)
    std::string clean;
    clean.reserve(out.size());
    bool prev_space = false;
    for (char c : out) {
        if (c == ' ') {
            if (!prev_space) { clean += ' '; prev_space = true; }
        } else {
            clean += c;
            prev_space = false;
        }
    }
    // trim trailing
    while (!clean.empty() && clean.back() == ' ') clean.pop_back();
    return clean;
}

Json::Value EnrichController::buildOutputRecord(
    const Json::Value&                     raw,
    const ParsedAddress&                   parsed,
    const std::vector<std::string>&        expansions,
    const std::unordered_set<std::string>& normalized_cols,
    const std::string&                     addr_col)
{
    Json::Value out;
    const bool all = normalized_cols.empty(); // true = ALL mode

    // -----------------------------------------------------------------
    //  ALL mode  → copy every original field as-is, then append parsed
    //  Specific  → copy only the requested fields (from raw or parsed)
    // -----------------------------------------------------------------
    // Always copy ALL original raw fields untouched (id, name, phone, addr, etc.)
    // normalized_cols only controls which *parsed* fields get appended below.
    out = raw;

    // Helper: should we include this parsed field?
    auto include = [&](const std::string& f) -> bool {
        return all || normalized_cols.count(f);
    };

    // Parsed address components
    if (include("house_number")  && !parsed.house_number.empty())
        out["house_number"]   = parsed.house_number;
    if (include("road")          && !parsed.road.empty())
        out["road"]           = parsed.road;
    if (include("suburb")        && !parsed.suburb.empty())
        out["suburb"]         = parsed.suburb;
    if (include("city")          && !parsed.city.empty())
        out["city"]           = parsed.city;
    if (include("city_district") && !parsed.city_district.empty())
        out["city_district"]  = parsed.city_district;
    if (include("state")         && !parsed.state.empty())
        out["state"]          = parsed.state;
    if (include("state_district")&& !parsed.state_district.empty())
        out["state_district"] = parsed.state_district;
    if (include("postcode")      && !parsed.postcode.empty())
        out["postcode"]       = parsed.postcode;
    if (include("country")       && !parsed.country.empty())
        out["country"]        = parsed.country;
    if (include("unit")          && !parsed.unit.empty())
        out["unit"]           = parsed.unit;
    if (include("level")         && !parsed.level.empty())
        out["level"]          = parsed.level;
    if (include("po_box")        && !parsed.po_box.empty())
        out["po_box"]         = parsed.po_box;

    // Metadata — always included
    out["confidence"]  = parsed.confidence;
    out["from_cache"]  = parsed.from_cache;
    out["latency_ms"]  = parsed.latency_ms;
    if (!parsed.error.empty()) out["parse_error"] = parsed.error;

    // normalized address string — field name is "normalized_<addr_col>"
    if (parsed.error.empty()) {
        std::string norm_key = "normalized_" + addr_col;
        out[norm_key] = buildNormalizedField(parsed);
    }

    // Expansions — top MAX_EXPANSIONS (always included)
    Json::Value exp_arr(Json::arrayValue);
    int n = std::min(static_cast<int>(expansions.size()), MAX_EXPANSIONS);
    for (int i = 0; i < n; ++i) exp_arr.append(expansions[i]);
    out["expansions"] = exp_arr;

    return out;
}

// =============================================================================
//  writeJsonDebug — only called when WRITE_JSON_DEBUG = true
// =============================================================================
void EnrichController::writeJsonDebug(const Json::Value& results) {
    if constexpr (!WRITE_JSON_DEBUG) return;

    Json::StreamWriterBuilder wb;
    wb["indentation"] = "  ";
    std::ofstream f("./uploads/debug_results.json", std::ios::trunc);
    if (f.is_open()) f << Json::writeString(wb, results);
}

// =============================================================================
//  writeCsvDebug — only called when WRITE_CSV_DEBUG = true
//  Writes a flat CSV: one row per record, all JSON keys as columns.
// =============================================================================
void EnrichController::writeCsvDebug(const Json::Value& results,
                                      const std::vector<std::string>& all_keys) {
    if constexpr (!WRITE_CSV_DEBUG) return;
    if (!results.isArray() || results.empty()) return;

    std::ofstream f("./uploads/debug_results.csv", std::ios::trunc);
    if (!f.is_open()) return;

    // Header row
    for (size_t i = 0; i < all_keys.size(); ++i) {
        if (i) f << ',';
        f << '"' << all_keys[i] << '"';
    }
    f << '\n';

    // Data rows
    for (const auto& rec : results) {
        for (size_t i = 0; i < all_keys.size(); ++i) {
            if (i) f << ',';
            const auto& k = all_keys[i];
            if (!rec.isMember(k)) { f << "\"\""; continue; }

            const auto& v = rec[k];
            if (v.isArray()) {
                // expansions etc — join with |
                std::string joined;
                for (const auto& el : v) {
                    if (!joined.empty()) joined += '|';
                    joined += el.asString();
                }
                f << '"' << joined << '"';
            } else if (v.isBool()) {
                f << (v.asBool() ? "true" : "false");
            } else if (v.isNumeric()) {
                f << v.asString();
            } else {
                // String: escape internal quotes
                // CSV double-quote escape: replace " with ""
                std::string s = v.asString();
                std::string escaped;
                escaped.reserve(s.size());
                for (char c : s) {
                    if (c == '"') escaped += '"'; // extra quote
                    escaped += c;
                }
                f << '"' << escaped << '"';
            }
        }
        f << '\n';
    }
}

// =============================================================================
//  enrich — main handler
//
//  Pipeline per record:
//    validate → sanitize → cache check → preprocess → libpostal parse
//    → confidence score → rule engine → (LLM if enabled)
//    → normalize (expand_address) → build output record
// =============================================================================
void EnrichController::enrich(const drogon::HttpRequestPtr& req,
                               std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
    ScopedTimer total_timer;
    auto& metrics = MetricsCollector::instance();

    // -------------------------------------------------------------------------
    //  1. Parse request body
    // -------------------------------------------------------------------------
    auto json = req->getJsonObject();
    if (!json) {
        Json::Value err;
        err["error"] = "Invalid JSON body";
        auto r = drogon::HttpResponse::newHttpJsonResponse(err);
        r->setStatusCode(drogon::k400BadRequest);
        callback(r);
        return;
    }

    if (!json->isMember("data") || !(*json)["data"].isArray()) {
        Json::Value err;
        err["error"] = "Missing or invalid 'data' array";
        auto r = drogon::HttpResponse::newHttpJsonResponse(err);
        r->setStatusCode(drogon::k400BadRequest);
        callback(r);
        return;
    }

    const Json::Value& records = (*json)["data"];
    const int N = static_cast<int>(records.size());

    if (N == 0) {
        Json::Value err; err["error"] = "Empty records array";
        auto r = drogon::HttpResponse::newHttpJsonResponse(err);
        r->setStatusCode(drogon::k400BadRequest);
        callback(r); return;
    }
    if (N > MAX_RECORDS) {
        Json::Value err;
        err["error"] = "records count " + std::to_string(N) +
                       " exceeds maximum " + std::to_string(MAX_RECORDS);
        auto r = drogon::HttpResponse::newHttpJsonResponse(err);
        r->setStatusCode(drogon::k400BadRequest);
        callback(r); return;
    }

    // -------------------------------------------------------------------------
    //  2. Resolve params from body
    // -------------------------------------------------------------------------
    const Json::Value& first = records[0];

    // metadata block (optional — falls back to top-level keys for compatibility)
    const Json::Value* meta = nullptr;
    if (json->isMember("metadata") && (*json)["metadata"].isObject())
        meta = &(*json)["metadata"];

    auto metaGet = [&](const char* key) -> std::string {
        if (meta && meta->isMember(key) && (*meta)[key].isString())
            return (*meta)[key].asString();
        if (json->isMember(key) && (*json)[key].isString())
            return (*json)[key].asString();
        return "";
    };

    // address_column
    std::string addr_col = metaGet("address_column");
    if (addr_col.empty()) addr_col = detectAddressColumn(first);
    if (addr_col.empty()) {
        Json::Value err; err["error"] = "Cannot detect address column. Pass 'address_column'.";
        auto r = drogon::HttpResponse::newHttpJsonResponse(err);
        r->setStatusCode(drogon::k400BadRequest);
        callback(r); return;
    }

    std::string key_col = metaGet("key_column");
    if (key_col.empty()) key_col = detectKeyColumn(first, addr_col);

    std::string norm_spec = metaGet("normalized_columns");
    if (norm_spec.empty()) norm_spec = "ALL";

    std::string lang = metaGet("language");
    if (lang.empty()) lang = g_config.default_language;
    std::string ctry = metaGet("country");
    if (ctry.empty()) ctry = g_config.default_country;

    auto norm_cols = parseNormalizeColumns(norm_spec);

    // -------------------------------------------------------------------------
    //  3. Process each record
    // -------------------------------------------------------------------------
    Json::Value output(Json::arrayValue);
    int succeeded = 0, failed = 0;

    // Collect all output keys for CSV header (built incrementally)
    std::vector<std::string> csv_key_order;
    std::unordered_set<std::string> csv_keys_seen;

    for (int i = 0; i < N; ++i) {
        const Json::Value& raw = records[i];

        // Address value
        std::string raw_addr;
        if (raw.isMember(addr_col) && raw[addr_col].isString())
            raw_addr = raw[addr_col].asString();

        // Validate
        ParsedAddress parsed;
        std::vector<std::string> expansions;

        auto validation = InputValidator::validateAddress(raw_addr);
        if (!validation.valid) {
            parsed.raw_input = raw_addr;
            parsed.error     = validation.error;
            ++failed;
        } else {
            std::string sanitized = InputValidator::sanitize(raw_addr);

            // Cache check
            bool cache_hit = false;
            if (g_config.cache_enabled && g_cache) {
                auto cached = g_cache->get(sanitized);
                if (cached.has_value()) {
                    parsed = *cached;
                    parsed.from_cache = true;
                    cache_hit = true;
                    metrics.recordCacheHit();
                }
            }

            if (!cache_hit) {
                if (g_config.cache_enabled) metrics.recordCacheMiss();

                // Phase 1: pre-process
                std::string cleaned = g_preprocessor.process(sanitized);
                metrics.recordPhase(1);

                // Phase 2: libpostal parse
                parsed = g_parser.parse(cleaned, lang, ctry);
                parsed.raw_input = sanitized;
                metrics.recordPhase(2);

                if (parsed.error.empty()) {
                    parsed.confidence = g_scorer.score(parsed);
                    metrics.recordConfidence(parsed.confidence);

                    // Phase 3: rule engine
                    if (g_config.rules_enabled &&
                        parsed.confidence < g_config.llm_confidence_threshold) {
                        if (g_rules.apply(parsed)) {
                            parsed.confidence = g_scorer.score(parsed);
                            metrics.recordPhase(3);
                        }
                    }

                    // Phase 4: LLM fallback
                    if (g_config.llm_enabled && g_llm.isReady() &&
                        parsed.confidence < g_config.llm_low_threshold) {
                        ScopedTimer llm_t;
                        if (g_llm.improve(parsed, g_config.llm_timeout_ms)) {
                            parsed.confidence = g_scorer.score(parsed);
                        }
                        metrics.recordLLMFallback(llm_t.elapsedMs());
                        metrics.recordPhase(4);
                    }

                    // Cache store
                    if (g_config.cache_enabled && g_cache &&
                        parsed.confidence >= g_config.cache_min_confidence)
                        g_cache->put(sanitized, parsed);
                }
            }

            // Normalize / expand (top MAX_EXPANSIONS)
            if (parsed.error.empty()) {
                std::string to_expand = parsed.road.empty() ? sanitized : parsed.road;
                NormalizedAddress norm = g_parser.normalize(to_expand, lang, ctry);
                int n = std::min(static_cast<int>(norm.normalizations.size()), MAX_EXPANSIONS);
                expansions.assign(norm.normalizations.begin(),
                                  norm.normalizations.begin() + n);
            }

            parsed.error.empty() ? ++succeeded : ++failed;
        }

        Json::Value out_rec = buildOutputRecord(raw, parsed, expansions, norm_cols, addr_col);
        output.append(out_rec);

        // Track CSV column order (preserves first-seen order)
        if constexpr (WRITE_CSV_DEBUG) {
            for (const auto& k : out_rec.getMemberNames()) {
                if (!csv_keys_seen.count(k)) {
                    csv_keys_seen.insert(k);
                    csv_key_order.push_back(k);
                }
            }
        }
    }

    // -------------------------------------------------------------------------
    //  4. Build response envelope
    // -------------------------------------------------------------------------
    Json::Value resp_body;
    resp_body["total"]            = N;
    resp_body["succeeded"]        = succeeded;
    resp_body["failed"]           = failed;
    resp_body["address_column"]   = addr_col;
    resp_body["key_column"]       = key_col.empty() ? Json::Value(Json::nullValue)
                                                     : Json::Value(key_col);
    resp_body["normalized_columns"]= norm_spec;
    resp_body["total_latency_ms"] = total_timer.elapsedMs();
    resp_body["results"]          = output;

    metrics.recordBatch(N, total_timer.elapsedMs());

    // -------------------------------------------------------------------------
    //  5. Debug file writes (compiled out when both flags are false)
    // -------------------------------------------------------------------------
    if constexpr (WRITE_JSON_DEBUG) writeJsonDebug(output);
    if constexpr (WRITE_CSV_DEBUG)  writeCsvDebug(output, csv_key_order);

    auto r = drogon::HttpResponse::newHttpJsonResponse(resp_body);
    callback(r);
}

} // namespace addr