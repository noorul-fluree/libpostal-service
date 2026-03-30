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
#include <iostream>

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

static const std::vector<std::string> kKeyCandidates = {
    "id", "uid", "uuid", "key", "pk", "record_id", "row_id", "_id"
};

// =============================================================================
//  detectAddressColumn
// =============================================================================
std::string EnrichController::detectAddressColumn(const Json::Value& rec) {
    if (!rec.isObject()) return "";
    for (const auto& cand : kAddrCandidates) {
        if (rec.isMember(cand) && rec[cand].isString()) return cand;
        for (const auto& key : rec.getMemberNames()) {
            std::string lower = key;
            std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
            if (lower == cand && rec[key].isString()) return key;
        }
    }
    // fallback: longest string field
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
    for (const auto& key : rec.getMemberNames())
        if (key != addr_col && (rec[key].isInt() || rec[key].isUInt())) return key;
    for (const auto& key : rec.getMemberNames())
        if (key != addr_col) return key;
    return "";
}

// =============================================================================
//  parseNormalizeColumns
// =============================================================================
std::unordered_set<std::string>
EnrichController::parseNormalizeColumns(const std::string& spec) {
    std::unordered_set<std::string> cols;
    if (spec.empty() || spec == "ALL") return cols;
    std::istringstream ss(spec);
    std::string token;
    while (std::getline(ss, token, ',')) {
        token.erase(0, token.find_first_not_of(" \t"));
        token.erase(token.find_last_not_of(" \t") + 1);
        if (!token.empty()) cols.insert(token);
    }
    return cols;
}

// =============================================================================
//  buildNormalizedField
// =============================================================================
static std::string buildNormalizedField(const ParsedAddress& p) {
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
        for (unsigned char c : *f) {
            if (std::isalnum(c) || c == ' ') out += static_cast<char>(std::tolower(c));
            else if (c >= 0x80)              out += static_cast<char>(c);
        }
    }
    // collapse double spaces
    std::string clean;
    clean.reserve(out.size());
    bool prev_space = false;
    for (char c : out) {
        if (c == ' ') { if (!prev_space) { clean += ' '; prev_space = true; } }
        else          { clean += c; prev_space = false; }
    }
    while (!clean.empty() && clean.back() == ' ') clean.pop_back();
    return clean;
}

// =============================================================================
//  buildOutputRecord
// =============================================================================
Json::Value EnrichController::buildOutputRecord(
    const Json::Value&                     raw,
    const ParsedAddress&                   parsed,
    const std::vector<std::string>&        expansions,
    const std::unordered_set<std::string>& normalize_cols,
    const std::string&                     addr_col)
{
    // Always copy ALL original raw fields untouched
    Json::Value out = raw;
    const bool all = normalize_cols.empty();

    auto include = [&](const std::string& f) -> bool {
        return all || normalize_cols.count(f);
    };

    if (include("house_number")  && !parsed.house_number.empty())  out["house_number"]   = parsed.house_number;
    if (include("road")          && !parsed.road.empty())           out["road"]           = parsed.road;
    if (include("suburb")        && !parsed.suburb.empty())         out["suburb"]         = parsed.suburb;
    if (include("city")          && !parsed.city.empty())           out["city"]           = parsed.city;
    if (include("city_district") && !parsed.city_district.empty())  out["city_district"]  = parsed.city_district;
    if (include("state")         && !parsed.state.empty())          out["state"]          = parsed.state;
    if (include("state_district")&& !parsed.state_district.empty()) out["state_district"] = parsed.state_district;
    if (include("postcode")      && !parsed.postcode.empty())       out["postcode"]       = parsed.postcode;
    if (include("country")       && !parsed.country.empty())        out["country"]        = parsed.country;
    if (include("unit")          && !parsed.unit.empty())           out["unit"]           = parsed.unit;
    if (include("level")         && !parsed.level.empty())          out["level"]          = parsed.level;
    if (include("po_box")        && !parsed.po_box.empty())         out["po_box"]         = parsed.po_box;

    out["confidence"] = parsed.confidence;
    out["from_cache"] = parsed.from_cache;
    out["latency_ms"] = parsed.latency_ms;

    if (!parsed.error.empty()) out["parse_error"] = parsed.error;

    if (parsed.error.empty()) {
        std::string norm_key = "normalize_" + addr_col;
        out[norm_key] = buildNormalizedField(parsed);
    }

    Json::Value exp_arr(Json::arrayValue);
    int n = std::min(static_cast<int>(expansions.size()), MAX_EXPANSIONS);
    for (int i = 0; i < n; ++i) exp_arr.append(expansions[i]);
    out["expansions"] = exp_arr;

    return out;
}

// =============================================================================
//  writeJsonDebug
// =============================================================================
void EnrichController::writeJsonDebug(const Json::Value& results) {
    if constexpr (!WRITE_JSON_DEBUG) return;
    Json::StreamWriterBuilder wb;
    wb["indentation"] = "  ";
    std::ofstream f("./uploads/debug_results.json", std::ios::trunc);
    if (f.is_open()) f << Json::writeString(wb, results);
}

// =============================================================================
//  writeCsvDebug
// =============================================================================
void EnrichController::writeCsvDebug(const Json::Value& results,
                                      const std::vector<std::string>& all_keys) {
    if constexpr (!WRITE_CSV_DEBUG) return;
    if (!results.isArray() || results.empty()) return;
    std::ofstream f("./uploads/debug_results.csv", std::ios::trunc);
    if (!f.is_open()) return;
    for (size_t i = 0; i < all_keys.size(); ++i) {
        if (i) f << ',';
        f << '"' << all_keys[i] << '"';
    }
    f << '\n';
    for (const auto& rec : results) {
        for (size_t i = 0; i < all_keys.size(); ++i) {
            if (i) f << ',';
            const auto& k = all_keys[i];
            if (!rec.isMember(k)) { f << "\"\""; continue; }
            const auto& v = rec[k];
            if (v.isArray()) {
                std::string joined;
                for (const auto& el : v) { if (!joined.empty()) joined += '|'; joined += el.asString(); }
                f << '"' << joined << '"';
            } else if (v.isBool()) {
                f << (v.asBool() ? "true" : "false");
            } else if (v.isNumeric()) {
                f << v.asString();
            } else {
                std::string s = v.asString();
                std::string escaped; escaped.reserve(s.size());
                for (char c : s) { if (c == '"') escaped += '"'; escaped += c; }
                f << '"' << escaped << '"';
            }
        }
        f << '\n';
    }
}

// =============================================================================
//  enrich — main handler
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
        std::cout << "[EnrichController] ✘ POST /api/v1/enrich | error=Invalid JSON body\n";
        Json::Value err; err["error"] = "Invalid JSON body";
        auto r = drogon::HttpResponse::newHttpJsonResponse(err);
        r->setStatusCode(drogon::k400BadRequest);
        callback(r); return;
    }

    if (!json->isMember("data") || !(*json)["data"].isArray()) {
        std::cout << "[EnrichController] ✘ POST /api/v1/enrich | error=Missing or invalid 'data' array\n";
        Json::Value err; err["error"] = "Missing or invalid 'data' array";
        auto r = drogon::HttpResponse::newHttpJsonResponse(err);
        r->setStatusCode(drogon::k400BadRequest);
        callback(r); return;
    }

    const Json::Value& records = (*json)["data"];
    const int N = static_cast<int>(records.size());

    if (N == 0) {
        std::cout << "[EnrichController] ✘ POST /api/v1/enrich | error=Empty records array\n";
        Json::Value err; err["error"] = "Empty records array";
        auto r = drogon::HttpResponse::newHttpJsonResponse(err);
        r->setStatusCode(drogon::k400BadRequest);
        callback(r); return;
    }

    bool truncated    = false;
    int  total_received = N;
    int  process_N    = N;
    if (N > g_config.batch_max_size) {
        process_N  = g_config.batch_max_size;
        truncated  = true;
    }

    std::cout << "[EnrichController] ▶ POST /api/v1/enrich | records_in=" << N
              << " | processing=" << process_N
              << " | max_allowed=" << g_config.batch_max_size << "\n";

    // -------------------------------------------------------------------------
    //  2. Resolve params
    // -------------------------------------------------------------------------
    const Json::Value& first = records[0];

    const Json::Value* meta = nullptr;
    if (json->isMember("metadata") && (*json)["metadata"].isObject())
        meta = &(*json)["metadata"];

    auto metaGet = [&](const char* key) -> std::string {
        if (meta && meta->isMember(key) && (*meta)[key].isString()) return (*meta)[key].asString();
        if (json->isMember(key) && (*json)[key].isString()) return (*json)[key].asString();
        return "";
    };

    std::string addr_col = metaGet("address_column");
    if (addr_col.empty()) addr_col = detectAddressColumn(first);
    if (addr_col.empty()) {
        std::cout << "[EnrichController] ✘ POST /api/v1/enrich | error=Cannot detect address column\n";
        Json::Value err; err["error"] = "Cannot detect address column. Pass 'address_column' in metadata.";
        auto r = drogon::HttpResponse::newHttpJsonResponse(err);
        r->setStatusCode(drogon::k400BadRequest);
        callback(r); return;
    }

    std::string key_col  = metaGet("key_column");
    if (key_col.empty()) key_col = detectKeyColumn(first, addr_col);

    std::string norm_spec = metaGet("normalize_columns");
    if (norm_spec.empty()) norm_spec = "ALL";
    auto norm_cols = parseNormalizeColumns(norm_spec);

    std::string lang = metaGet("language");
    if (lang.empty()) lang = g_config.default_language;
    std::string ctry = metaGet("country");
    if (ctry.empty()) ctry = g_config.default_country;

    std::cout << "[EnrichController]   addr_col=" << addr_col
              << " | key_col=" << (key_col.empty() ? "(auto)" : key_col)
              << " | normalize_columns=" << norm_spec << "\n";

    // -------------------------------------------------------------------------
    //  3. Process each record
    // -------------------------------------------------------------------------
    Json::Value output(Json::arrayValue);
    int succeeded = 0, failed = 0;
    uint64_t cache_hits_before  = metrics.total_cache_hits.load();
    uint64_t cache_misses_before = metrics.total_cache_misses.load();

    std::vector<std::string> csv_key_order;
    std::unordered_set<std::string> csv_keys_seen;

    // for (int i = 0; i < N; ++i) {
     for (int i = 0; i < process_N; ++i) {
        const Json::Value& raw = records[i];
        std::string raw_addr;
        if (raw.isMember(addr_col) && raw[addr_col].isString())
            raw_addr = raw[addr_col].asString();

        ParsedAddress parsed;
        std::vector<std::string> expansions;

        auto validation = InputValidator::validateAddress(raw_addr);
        if (!validation.valid) {
            parsed.raw_input = raw_addr;
            parsed.error     = validation.error;
            ++failed;
        } else {
            std::string sanitized = InputValidator::sanitize(raw_addr);

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

                std::string cleaned = g_preprocessor.process(sanitized);
                metrics.recordPhase(1);

                parsed = g_parser.parse(cleaned, lang, ctry);
                parsed.raw_input = sanitized;
                metrics.recordPhase(2);

                if (parsed.error.empty()) {
                    parsed.confidence = g_scorer.score(parsed);
                    metrics.recordConfidence(parsed.confidence);

                    if (g_config.rules_enabled &&
                        parsed.confidence < g_config.llm_confidence_threshold) {
                        if (g_rules.apply(parsed)) {
                            parsed.confidence = g_scorer.score(parsed);
                            metrics.recordPhase(3);
                        }
                    }

                    if (g_config.llm_enabled && g_llm.isReady() &&
                        parsed.confidence < g_config.llm_low_threshold) {
                        ScopedTimer llm_t;
                        if (g_llm.improve(parsed, g_config.llm_timeout_ms))
                            parsed.confidence = g_scorer.score(parsed);
                        metrics.recordLLMFallback(llm_t.elapsedMs());
                        metrics.recordPhase(4);
                    }

                    if (g_config.cache_enabled && g_cache &&
                        parsed.confidence >= g_config.cache_min_confidence)
                        g_cache->put(sanitized, parsed);
                }
            }

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

        if constexpr (WRITE_CSV_DEBUG) {
            for (const auto& k : out_rec.getMemberNames()) {
                if (!csv_keys_seen.count(k)) {
                    csv_keys_seen.insert(k);
                    csv_key_order.push_back(k);
                }
            }
        }
    }

    double latency = total_timer.elapsedMs();
    uint64_t hits  = metrics.total_cache_hits.load()   - cache_hits_before;
    uint64_t misses= metrics.total_cache_misses.load() - cache_misses_before;

    // -------------------------------------------------------------------------
    //  4. Final request log
    // -------------------------------------------------------------------------
    std::cout << "[EnrichController] ✔ POST /api/v1/enrich"
              << " | records_in="  << N
              << " | succeeded="   << succeeded
              << " | failed="      << failed
              << " | cache_hits="  << hits
              << " | cache_misses="<< misses
              << " | latency="     << latency << "ms\n";

    if (failed > 0)
        std::cout << "[EnrichController]   ✘ " << failed << " record(s) failed validation\n";

    // -------------------------------------------------------------------------
    //  5. Build response
    // -------------------------------------------------------------------------
    // Json::Value resp_body;
    // resp_body["total"]             = N;
    // resp_body["succeeded"]         = succeeded;
    // resp_body["failed"]            = failed;
    // resp_body["address_column"]    = addr_col;
    // resp_body["key_column"]        = key_col.empty() ? Json::Value(Json::nullValue) : Json::Value(key_col);
    // resp_body["normalize_columns"] = norm_spec;
    // resp_body["total_latency_ms"]  = latency;
    // resp_body["results"]           = output;

    Json::Value resp_body;
    resp_body["total"]             = process_N;
    resp_body["succeeded"]         = succeeded;
    resp_body["failed"]            = failed;
    resp_body["address_column"]    = addr_col;
    resp_body["key_column"]        = key_col.empty() ? Json::Value(Json::nullValue) : Json::Value(key_col);
    resp_body["normalize_columns"] = norm_spec;
    resp_body["total_latency_ms"]  = latency;
    resp_body["results"]           = output;
    if (truncated) {
        resp_body["warning"] = "Received " + std::to_string(total_received) +
                               " records but maximum is " +
                               std::to_string(g_config.batch_max_size) +
                               ". Only first " + std::to_string(process_N) +
                               " records processed. Remaining " +
                               std::to_string(total_received - process_N) +
                               " records were discarded.";
        resp_body["total_received"]  = total_received;
        resp_body["total_processed"] = process_N;
        resp_body["total_discarded"] = total_received - process_N;
    }

    metrics.recordBatch(N, latency);

    if constexpr (WRITE_JSON_DEBUG) writeJsonDebug(output);
    if constexpr (WRITE_CSV_DEBUG)  writeCsvDebug(output, csv_key_order);

    auto r = drogon::HttpResponse::newHttpJsonResponse(resp_body);
    callback(r);
}

} // namespace addr