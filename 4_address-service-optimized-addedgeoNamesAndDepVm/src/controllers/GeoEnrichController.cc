#include "controllers/GeoEnrichController.h"
#include "services/AddressParser.h"
#include "services/PreProcessor.h"
#include "services/ConfidenceScorer.h"
#include "services/RuleEngine.h"
#include "services/CacheManager.h"
#include "services/LLMFallback.h"
#include "services/MetricsCollector.h"
#include "services/GeoNamesDB.h"
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
//  Country name → ISO code mapping (for resolving country hint)
// =============================================================================
static std::string countryNameToCode(const std::string& name) {
    static const std::unordered_map<std::string, std::string> kMap = {
        {"india","IN"}, {"in","IN"}, {"ind","IN"},
        {"united states","US"}, {"usa","US"}, {"us","US"}, {"united states of america","US"},
        {"united kingdom","GB"}, {"uk","GB"}, {"gb","GB"}, {"great britain","GB"}, {"england","GB"},
    };
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    auto it = kMap.find(lower);
    return it != kMap.end() ? it->second : "";
}

// =============================================================================
//  extractOutwardCode — for UK postcodes, extract just the outward part
//  "sw1a 1aa" → "SW1A"    "ec1a 1bb" → "EC1A"    "m1 1ae" → "M1"
//  For non-UK postcodes returns uppercased input unchanged
// =============================================================================
static std::string extractOutwardCode(const std::string& postcode) {
    std::string upper = postcode;
    std::transform(upper.begin(), upper.end(), upper.begin(), ::toupper);
    // Remove trailing spaces
    while (!upper.empty() && upper.back() == ' ') upper.pop_back();

    // UK postcode has a space in the middle: "SW1A 1AA"
    // Take everything before the space
    auto space = upper.find(' ');
    if (space != std::string::npos) return upper.substr(0, space);
    return upper;
}

// =============================================================================
//  isUKPostcode — rough check: starts with 1-2 alpha chars
// =============================================================================
static bool looksLikeUKPostcode(const std::string& pc) {
    if (pc.size() < 2) return false;
    // UK postcodes start with letters
    return std::isalpha(static_cast<unsigned char>(pc[0])) &&
           !std::isdigit(static_cast<unsigned char>(pc[0]));
}

// =============================================================================
//  resolveCountryCode
//  Priority: parsed.country → UK postcode detection → request hint → empty
// =============================================================================
std::string GeoEnrichController::resolveCountryCode(const ParsedAddress& parsed,
                                                      const std::string&   hint) {
    if (!parsed.country.empty()) {
        std::string cc = countryNameToCode(parsed.country);
        if (!cc.empty()) return cc;
    }
    // Auto-detect GB from postcode pattern (libpostal often misses country for UK)
    if (!parsed.postcode.empty() && looksLikeUKPostcode(parsed.postcode)) {
        return "GB";
    }
    if (!hint.empty()) {
        std::string cc = countryNameToCode(hint);
        if (!cc.empty()) return cc;
        std::string upper = hint;
        std::transform(upper.begin(), upper.end(), upper.begin(), ::toupper);
        if (upper == "IN" || upper == "US" || upper == "GB") return upper;
    }
    return "";
}

// =============================================================================
//  applyGeoLookup — Phase 2A + 2B
//
//  Phase 2A: if postcode is present → lookup postal→city+state
//  Phase 2B: if city is present but state is missing → lookup city→state
//
//  Fill strategy:
//    - Always fill blank fields
//    - Override if confidence < GEO_OVERRIDE_THRESHOLD (0.7)
// =============================================================================
bool GeoEnrichController::applyGeoLookup(ParsedAddress&     parsed,
                                           double             confidence,
                                           const std::string& cc,
                                           std::string&       geo_source) {
    if (cc.empty()) { geo_source = "none"; return false; }

    auto& geo = GeoNamesDB::instance();
    bool changed = false;
    bool should_override = (confidence < GeoNamesDB::GEO_OVERRIDE_THRESHOLD);

    // ------------------------------------------------------------------
    //  Phase 2A: postal code → city + state
    //  For UK: extract outward code only (e.g. "sw1a 1aa" → "SW1A")
    // ------------------------------------------------------------------
    if (!parsed.postcode.empty()) {
        std::string postal_key = parsed.postcode;
        if (cc == "GB") postal_key = extractOutwardCode(parsed.postcode);
        auto entry = geo.lookupPostal(postal_key, cc);
        if (entry.has_value()) {
            if (parsed.city.empty() || should_override) {
                if (parsed.city != entry->city && !entry->city.empty()) {
                    parsed.city = entry->city;
                    changed = true;
                }
            }
            if (parsed.state.empty() || should_override) {
                if (parsed.state != entry->state && !entry->state.empty()) {
                    parsed.state = entry->state;
                    changed = true;
                }
            }
            if (changed) { geo_source = "postal_2a"; return true; }
        }
    }

    // ------------------------------------------------------------------
    //  Phase 2B: city → state (if state still missing)
    // ------------------------------------------------------------------
    if (!parsed.city.empty() && (parsed.state.empty() || should_override)) {
        std::string state = geo.lookupCityState(parsed.city, cc);
        if (!state.empty() && parsed.state != state) {
            parsed.state = state;
            changed = true;
            geo_source = "city_2b";
            return true;
        }
    }

    geo_source = "none";
    return false;
}

// =============================================================================
//  Shared helpers (duplicated from EnrichController to keep independence)
// =============================================================================
static const std::vector<std::string> kGeoAddrCandidates = {
    "addr", "address", "full_address", "location"
};
static const std::vector<std::string> kGeoKeyCandidates = {
    "id", "uid", "uuid", "key", "pk", "record_id", "row_id", "_id"
};

std::string GeoEnrichController::detectAddressColumn(const Json::Value& rec) {
    if (!rec.isObject()) return "";
    for (const auto& cand : kGeoAddrCandidates) {
        if (rec.isMember(cand) && rec[cand].isString()) return cand;
        for (const auto& key : rec.getMemberNames()) {
            std::string lower = key;
            std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
            if (lower == cand && rec[key].isString()) return key;
        }
    }
    std::string best; size_t best_len = 0;
    for (const auto& key : rec.getMemberNames()) {
        if (rec[key].isString()) {
            size_t len = rec[key].asString().size();
            if (len > best_len) { best_len = len; best = key; }
        }
    }
    return best;
}

std::string GeoEnrichController::detectKeyColumn(const Json::Value& rec,
                                                   const std::string& addr_col) {
    if (!rec.isObject()) return "";
    for (const auto& cand : kGeoKeyCandidates) {
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

std::unordered_set<std::string>
GeoEnrichController::parseNormalizeColumns(const std::string& spec) {
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
//  buildNormalizedField (same logic as EnrichController)
// =============================================================================
static std::string buildGeoNormalizedField(const ParsedAddress& p) {
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
    std::string clean; clean.reserve(out.size());
    bool prev = false;
    for (char c : out) {
        if (c == ' ') { if (!prev) { clean += ' '; prev = true; } }
        else          { clean += c; prev = false; }
    }
    while (!clean.empty() && clean.back() == ' ') clean.pop_back();
    return clean;
}

// =============================================================================
//  buildOutputRecord
// =============================================================================
Json::Value GeoEnrichController::buildOutputRecord(
    const Json::Value&                     raw,
    const ParsedAddress&                   parsed,
    const std::vector<std::string>&        expansions,
    const std::unordered_set<std::string>& normalize_cols,
    const std::string&                     addr_col,
    const std::string&                     geo_source,
    bool                                   geo_applied)
{
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

    out["confidence"]  = parsed.confidence;
    out["from_cache"]  = parsed.from_cache;
    out["latency_ms"]  = parsed.latency_ms;
    out["geo_source"]  = geo_source;
    out["geo_applied"] = geo_applied;

    if (!parsed.error.empty()) out["parse_error"] = parsed.error;

    if (parsed.error.empty()) {
        std::string norm_key = "normalize_" + addr_col;
        out[norm_key] = buildGeoNormalizedField(parsed);
    }

    Json::Value exp_arr(Json::arrayValue);
    int n = std::min(static_cast<int>(expansions.size()), MAX_EXPANSIONS);
    for (int i = 0; i < n; ++i) exp_arr.append(expansions[i]);
    out["expansions"] = exp_arr;

    return out;
}

// =============================================================================
//  writeJsonDebug / writeCsvDebug
// =============================================================================
void GeoEnrichController::writeJsonDebug(const Json::Value& results) {
    if constexpr (!WRITE_JSON_DEBUG) return;
    Json::StreamWriterBuilder wb; wb["indentation"] = "  ";
    std::ofstream f("./uploads/geo_debug_results.json", std::ios::trunc);
    if (f.is_open()) f << Json::writeString(wb, results);
}

void GeoEnrichController::writeCsvDebug(const Json::Value& results,
                                         const std::vector<std::string>& all_keys) {
    if constexpr (!WRITE_CSV_DEBUG) return;
    if (!results.isArray() || results.empty()) return;
    std::ofstream f("./uploads/geo_debug_results.csv", std::ios::trunc);
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
            } else if (v.isBool())    { f << (v.asBool() ? "true" : "false"); }
            else if (v.isNumeric())   { f << v.asString(); }
            else {
                std::string s = v.asString(), esc; esc.reserve(s.size());
                for (char c : s) { if (c == '"') esc += '"'; esc += c; }
                f << '"' << esc << '"';
            }
        }
        f << '\n';
    }
}

// =============================================================================
//  geoEnrich — main handler
// =============================================================================
void GeoEnrichController::geoEnrich(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback)
{
    ScopedTimer total_timer;
    auto& metrics = MetricsCollector::instance();
    auto& geo     = GeoNamesDB::instance();

    // -------------------------------------------------------------------------
    //  1. Parse request body
    // -------------------------------------------------------------------------
    auto json = req->getJsonObject();
    if (!json) {
        Json::Value err; err["error"] = "Invalid JSON body";
        auto r = drogon::HttpResponse::newHttpJsonResponse(err);
        r->setStatusCode(drogon::k400BadRequest);
        callback(r); return;
    }

    if (!json->isMember("data") || !(*json)["data"].isArray()) {
        Json::Value err; err["error"] = "Missing or invalid 'data' array";
        auto r = drogon::HttpResponse::newHttpJsonResponse(err);
        r->setStatusCode(drogon::k400BadRequest);
        callback(r); return;
    }

    const Json::Value& records = (*json)["data"];
    int  total_received = static_cast<int>(records.size());
    bool truncated      = false;
    int  process_N      = total_received;

    if (total_received == 0) {
        Json::Value err; err["error"] = "Empty records array";
        auto r = drogon::HttpResponse::newHttpJsonResponse(err);
        r->setStatusCode(drogon::k400BadRequest);
        callback(r); return;
    }

    if (total_received > MAX_RECORDS) {
        process_N = MAX_RECORDS;
        truncated = true;
    }

    // -------------------------------------------------------------------------
    //  2. Resolve params
    // -------------------------------------------------------------------------
    const Json::Value& first = records[0];
    const Json::Value* meta  = nullptr;
    if (json->isMember("metadata") && (*json)["metadata"].isObject())
        meta = &(*json)["metadata"];

    auto metaGet = [&](const char* key) -> std::string {
        if (meta && meta->isMember(key) && (*meta)[key].isString()) return (*meta)[key].asString();
        if (json->isMember(key) && (*json)[key].isString()) return (*json)[key].asString();
        return "";
    };

    std::string addr_col  = metaGet("address_column");
    if (addr_col.empty()) addr_col = detectAddressColumn(first);
    if (addr_col.empty()) {
        Json::Value err; err["error"] = "Cannot detect address column. Pass 'address_column' in metadata.";
        auto r = drogon::HttpResponse::newHttpJsonResponse(err);
        r->setStatusCode(drogon::k400BadRequest);
        callback(r); return;
    }

    std::string key_col   = metaGet("key_column");
    if (key_col.empty())  key_col = detectKeyColumn(first, addr_col);

    std::string norm_spec = metaGet("normalize_columns");
    if (norm_spec.empty()) norm_spec = "ALL";
    auto norm_cols = parseNormalizeColumns(norm_spec);

    std::string lang      = metaGet("language");
    if (lang.empty())     lang = g_config.default_language;
    std::string ctry_hint = metaGet("country");
    if (ctry_hint.empty()) ctry_hint = g_config.default_country;

    std::cout << "[GeoEnrichController] ▶ POST /api/v1/enrich/geo"
              << " | records_in=" << total_received
              << " | processing=" << process_N
              << " | geo_db=" << (geo.isReady() ? "ready" : "NOT LOADED")
              << "\n";

    // -------------------------------------------------------------------------
    //  3. Process each record
    // -------------------------------------------------------------------------
    Json::Value output(Json::arrayValue);
    int succeeded = 0, failed = 0, geo_applied_count = 0;

    std::vector<std::string> csv_key_order;
    std::unordered_set<std::string> csv_keys_seen;

    for (int i = 0; i < process_N; ++i) {
        const Json::Value& raw = records[i];
        std::string raw_addr;
        if (raw.isMember(addr_col) && raw[addr_col].isString())
            raw_addr = raw[addr_col].asString();

        ParsedAddress parsed;
        std::vector<std::string> expansions;
        std::string geo_source = "none";
        bool geo_applied = false;

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

                // Phase 2 libpostal: parse
                parsed = g_parser.parse(cleaned, lang, ctry_hint);
                parsed.raw_input = sanitized;
                metrics.recordPhase(2);

                if (parsed.error.empty()) {
                    parsed.confidence = g_scorer.score(parsed);
                    metrics.recordConfidence(parsed.confidence);

                    // Rule engine
                    if (g_config.rules_enabled &&
                        parsed.confidence < g_config.llm_confidence_threshold) {
                        if (g_rules.apply(parsed))
                            parsed.confidence = g_scorer.score(parsed);
                    }

                    // --------------------------------------------------------
                    //  Phase 2 GeoNames lookup (2A postal + 2B city→state)
                    // --------------------------------------------------------
                    if (geo.isReady()) {
                        std::string cc = resolveCountryCode(parsed, ctry_hint);
                        geo_applied = applyGeoLookup(parsed, parsed.confidence,
                                                     cc, geo_source);
                        if (geo_applied) {
                            // Rescore after geo enrichment
                            parsed.confidence = g_scorer.score(parsed);
                            ++geo_applied_count;
                        }
                    }

                    // LLM fallback
                    if (g_config.llm_enabled && g_llm.isReady() &&
                        parsed.confidence < g_config.llm_low_threshold) {
                        ScopedTimer llm_t;
                        if (g_llm.improve(parsed, g_config.llm_timeout_ms))
                            parsed.confidence = g_scorer.score(parsed);
                        metrics.recordLLMFallback(llm_t.elapsedMs());
                    }

                    // Cache store
                    if (g_config.cache_enabled && g_cache &&
                        parsed.confidence >= g_config.cache_min_confidence)
                        g_cache->put(sanitized, parsed);
                }
            }

            // Expansions
            if (parsed.error.empty()) {
                std::string to_expand = parsed.road.empty() ? sanitized : parsed.road;
                NormalizedAddress norm = g_parser.normalize(to_expand, lang, ctry_hint);
                int n = std::min(static_cast<int>(norm.normalizations.size()), MAX_EXPANSIONS);
                expansions.assign(norm.normalizations.begin(),
                                  norm.normalizations.begin() + n);
            }

            parsed.error.empty() ? ++succeeded : ++failed;
        }

        Json::Value out_rec = buildOutputRecord(raw, parsed, expansions,
                                                norm_cols, addr_col,
                                                geo_source, geo_applied);
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

    std::cout << "[GeoEnrichController] ✔ POST /api/v1/enrich/geo"
              << " | records_in="    << process_N
              << " | succeeded="     << succeeded
              << " | failed="        << failed
              << " | geo_applied="   << geo_applied_count
              << " | latency="       << latency << "ms\n";

    // -------------------------------------------------------------------------
    //  4. Build response
    // -------------------------------------------------------------------------
    Json::Value resp_body;
    resp_body["total"]             = process_N;
    resp_body["succeeded"]         = succeeded;
    resp_body["failed"]            = failed;
    resp_body["geo_applied_count"] = geo_applied_count;
    resp_body["geo_db_ready"]      = geo.isReady();
    resp_body["address_column"]    = addr_col;
    resp_body["key_column"]        = key_col.empty()
                                     ? Json::Value(Json::nullValue)
                                     : Json::Value(key_col);
    resp_body["normalize_columns"] = norm_spec;
    resp_body["total_latency_ms"]  = latency;
    resp_body["results"]           = output;

    if (truncated) {
        resp_body["warning"] = "Received " + std::to_string(total_received) +
                               " records but maximum is " +
                               std::to_string(MAX_RECORDS) +
                               ". Only first " + std::to_string(process_N) +
                               " records processed. Remaining " +
                               std::to_string(total_received - process_N) +
                               " records were discarded.";
        resp_body["total_received"]  = total_received;
        resp_body["total_processed"] = process_N;
        resp_body["total_discarded"] = total_received - process_N;
    }

    metrics.recordBatch(process_N, latency);

    if constexpr (WRITE_JSON_DEBUG) writeJsonDebug(output);
    if constexpr (WRITE_CSV_DEBUG)  writeCsvDebug(output, csv_key_order);

    auto r = drogon::HttpResponse::newHttpJsonResponse(resp_body);
    callback(r);
}

} // namespace addr