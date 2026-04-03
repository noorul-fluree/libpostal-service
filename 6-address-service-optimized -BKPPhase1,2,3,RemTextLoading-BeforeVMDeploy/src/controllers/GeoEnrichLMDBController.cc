#include "utils/Logger.h"
#include "controllers/GeoEnrichLMDBController.h"
#include "services/AddressParser.h"
#include "services/PreProcessor.h"
#include "services/ConfidenceScorer.h"
#include "services/RuleEngine.h"
#include "services/CacheManager.h"
#include "services/LLMFallback.h"
#include "services/MetricsCollector.h"
#include "services/GeoNamesLMDB.h"
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
//  Country name → ISO code
//  Uses LMDB countries database — all 249 countries supported
// =============================================================================
static std::string lmdbCountryNameToCode(const std::string& name) {
    if (name.empty()) return "";
    auto rec = GeoNamesLMDB::instance().lookupCountry(name);
    if (rec.has_value() && !rec->alpha2.empty()) return rec->alpha2;
    return "";
}

static bool lmdbLooksLikeUKPostcode(const std::string& pc) {
    if (pc.size() < 2) return false;
    return std::isalpha(static_cast<unsigned char>(pc[0])) &&
           !std::isdigit(static_cast<unsigned char>(pc[0]));
}

// =============================================================================
//  resolveCountryCode
// =============================================================================
std::string GeoEnrichLMDBController::resolveCountryCode(
    const ParsedAddress& parsed, const std::string& hint) {
    // Try parsed country name via LMDB
    if (!parsed.country.empty()) {
        std::string cc = lmdbCountryNameToCode(parsed.country);
        if (!cc.empty()) return cc;
    }
    // Auto-detect GB from postcode pattern
    if (!parsed.postcode.empty() && lmdbLooksLikeUKPostcode(parsed.postcode))
        return "GB";
    // Try hint via LMDB
    if (!hint.empty()) {
        std::string cc = lmdbCountryNameToCode(hint);
        if (!cc.empty()) return cc;
        // Trust any 2-letter code directly
        std::string upper = hint;
        std::transform(upper.begin(), upper.end(), upper.begin(), ::toupper);
        if (upper.size() == 2) return upper;
    }
    return "";
}

// =============================================================================
//  applyGeoLookup — uses all 6 GeoNamesLMDB databases
//
//  Phase 2A: postal → city + state        (postal.lmdb)
//  Phase 2B: city   → state               (cities.lmdb)
//  Phase 2C: alias resolution             (aliases.lmdb)
//  Phase 2D: country name fill            (countries.lmdb)
// =============================================================================
bool GeoEnrichLMDBController::applyGeoLookup(ParsedAddress&     parsed,
                                               double             confidence,
                                               const std::string& cc,
                                               std::string&       geo_source) {
    if (cc.empty()) { geo_source = "none"; return false; }

    auto& geo = GeoNamesLMDB::instance();
    bool changed = false;
    bool should_override = (confidence < GeoNamesLMDB::GEO_OVERRIDE_THRESHOLD);

    // ------------------------------------------------------------------
    //  Phase 2A: postal code → city + state  (postal.lmdb)
    // ------------------------------------------------------------------
    if (!parsed.postcode.empty()) {
        auto entry = geo.lookupPostal(parsed.postcode, cc);
        if (entry.has_value()) {
            if ((parsed.city.empty() || should_override) &&
                !entry->city.empty() && parsed.city != entry->city) {
                parsed.city = entry->city;
                changed = true;
            }
            if ((parsed.state.empty() || should_override) &&
                !entry->state.empty() && parsed.state != entry->state) {
                parsed.state = entry->state;
                changed = true;
            }
            if (changed) { geo_source = "postal_2a"; return true; }
        }
    }

    // ------------------------------------------------------------------
    //  Phase 2B: city → state  (cities.lmdb)
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

    // ------------------------------------------------------------------
    //  Phase 2C: alias resolution  (aliases.lmdb)
    //  e.g. "bombay" → "mumbai"
    // ------------------------------------------------------------------
    if (!parsed.city.empty()) {
        std::string city_lower = parsed.city;
        std::transform(city_lower.begin(), city_lower.end(),
                       city_lower.begin(), ::tolower);
        std::string canonical = geo.lookupAlias(city_lower, cc);
        if (!canonical.empty() && canonical != city_lower) {
            parsed.city = canonical;
            changed = true;
            geo_source = "alias_2c";
            return true;
        }
    }

    // ------------------------------------------------------------------
    //  Phase 2D: fill country name if empty  (countries.lmdb)
    // ------------------------------------------------------------------
    if (parsed.country.empty() && !cc.empty()) {
        auto crec = geo.lookupCountry(cc);
        if (crec.has_value() && !crec->name.empty()) {
            parsed.country = crec->name;
            changed = true;
            geo_source = "country_2d";
            return true;
        }
    }

    geo_source = "none";
    return false;
}

// =============================================================================
//  Shared helpers
// =============================================================================
static const std::vector<std::string> kLMDBAddrCandidates = {
    "addr", "address", "full_address", "location"
};
static const std::vector<std::string> kLMDBKeyCandidates = {
    "id", "uid", "uuid", "key", "pk", "record_id", "row_id", "_id"
};

std::string GeoEnrichLMDBController::detectAddressColumn(const Json::Value& rec) {
    if (!rec.isObject()) return "";
    for (const auto& cand : kLMDBAddrCandidates) {
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

std::string GeoEnrichLMDBController::detectKeyColumn(const Json::Value& rec,
                                                       const std::string& addr_col) {
    if (!rec.isObject()) return "";
    for (const auto& cand : kLMDBKeyCandidates) {
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
GeoEnrichLMDBController::parseNormalizeColumns(const std::string& spec) {
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

static std::string buildLMDBNormalizedField(const ParsedAddress& p) {
    const std::string* fields[] = {
        &p.house_number, &p.road,     &p.unit,    &p.level,
        &p.staircase,    &p.entrance, &p.po_box,
        &p.suburb,       &p.city_district, &p.city,
        &p.state,        &p.postcode, &p.country
    };
    std::string out; out.reserve(128);
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
Json::Value GeoEnrichLMDBController::buildOutputRecord(
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
    auto include = [&](const std::string& f) {
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

    out["confidence"]   = parsed.confidence;
    out["from_cache"]   = parsed.from_cache;
    out["latency_ms"]   = parsed.latency_ms;
    out["geo_source"]   = geo_source;
    out["geo_applied"]  = geo_applied;
    out["geo_backend"]  = std::string("lmdb");

    if (!parsed.error.empty()) out["parse_error"] = parsed.error;
    if (parsed.error.empty())
        out["normalize_" + addr_col] = buildLMDBNormalizedField(parsed);

    Json::Value exp_arr(Json::arrayValue);
    int n = std::min(static_cast<int>(expansions.size()), MAX_EXPANSIONS);
    for (int i = 0; i < n; ++i) exp_arr.append(expansions[i]);
    out["expansions"] = exp_arr;
    return out;
}

// =============================================================================
//  writeJsonDebug / writeCsvDebug
// =============================================================================
void GeoEnrichLMDBController::writeJsonDebug(const Json::Value& results) {
    if constexpr (!WRITE_JSON_DEBUG) return;
    Json::StreamWriterBuilder wb; wb["indentation"] = "  ";
    std::ofstream f("./uploads/lmdb_debug_results.json", std::ios::trunc);
    if (f.is_open()) f << Json::writeString(wb, results);
}

void GeoEnrichLMDBController::writeCsvDebug(const Json::Value& results,
                                              const std::vector<std::string>& keys) {
    if constexpr (!WRITE_CSV_DEBUG) return;
    if (!results.isArray() || results.empty()) return;
    std::ofstream f("./uploads/lmdb_debug_results.csv", std::ios::trunc);
    if (!f.is_open()) return;
    for (size_t i = 0; i < keys.size(); ++i) { if (i) f << ','; f << '"' << keys[i] << '"'; }
    f << '\n';
    for (const auto& rec : results) {
        for (size_t i = 0; i < keys.size(); ++i) {
            if (i) f << ',';
            if (!rec.isMember(keys[i])) { f << "\"\""; continue; }
            const auto& v = rec[keys[i]];
            if (v.isArray()) {
                std::string j;
                for (const auto& el : v) { if (!j.empty()) j += '|'; j += el.asString(); }
                f << '"' << j << '"';
            } else if (v.isBool())   { f << (v.asBool() ? "true" : "false"); }
            else if (v.isNumeric())  { f << v.asString(); }
            else {
                std::string s = v.asString(), e; e.reserve(s.size());
                for (char c : s) { if (c == '"') e += '"'; e += c; }
                f << '"' << e << '"';
            }
        }
        f << '\n';
    }
}

// =============================================================================
//  geoEnrichLMDB — main handler
// =============================================================================
void GeoEnrichLMDBController::geoEnrichLMDB(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback)
{
    ScopedTimer total_timer;
    auto& metrics = MetricsCollector::instance();
    auto& geo     = GeoNamesLMDB::instance();

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
    if (total_received > g_config.batch_max_size) { process_N = g_config.batch_max_size; truncated = true; }


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
        Json::Value err; err["error"] = "Cannot detect address column.";
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

    std::cout << "[GeoEnrichLMDB] ▶ POST /api/v1/enrich/geo/lmdb"
              << " | records_in=" << total_received
              << " | processing=" << process_N
              << " | lmdb=" << (geo.isReady() ? "ready" : "NOT LOADED") << "\n";


    LOG_F(INFO, "[enrich/geo/lmdb] ▶ records_in=%d processing=%d lmdb=%s",
      total_received, process_N, geo.isReady() ? "ready" : "NOT LOADED");\
      
      
    Json::Value output(Json::arrayValue);
    int succeeded = 0, failed = 0, geo_applied_count = 0;
    std::vector<std::string> csv_keys;
    std::unordered_set<std::string> csv_seen;

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

            bool cache_hit = false;
            if (g_config.cache_enabled && g_cache) {
                auto cached = g_cache->get(sanitized);
                if (cached.has_value()) {
                    parsed = *cached; parsed.from_cache = true;
                    cache_hit = true; metrics.recordCacheHit();
                }
            }

            if (!cache_hit) {
                if (g_config.cache_enabled) metrics.recordCacheMiss();
                std::string cleaned = g_preprocessor.process(sanitized);
                metrics.recordPhase(1);
                parsed = g_parser.parse(cleaned, lang, ctry_hint);
                parsed.raw_input = sanitized;
                metrics.recordPhase(2);

                if (parsed.error.empty()) {
                    parsed.confidence = g_scorer.score(parsed);
                    metrics.recordConfidence(parsed.confidence);

                    if (g_config.rules_enabled &&
                        parsed.confidence < g_config.llm_confidence_threshold)
                        if (g_rules.apply(parsed))
                            parsed.confidence = g_scorer.score(parsed);

                    // Phase 2 LMDB lookup (all 6 databases)
                    if (geo.isReady()) {
                        std::string cc = resolveCountryCode(parsed, ctry_hint);
                        geo_applied = applyGeoLookup(parsed, parsed.confidence,
                                                     cc, geo_source);
                        if (geo_applied) {
                            parsed.confidence = g_scorer.score(parsed);
                            ++geo_applied_count;
                        }
                    }

                    if (g_config.llm_enabled && g_llm.isReady() &&
                        parsed.confidence < g_config.llm_low_threshold) {
                        ScopedTimer llm_t;
                        if (g_llm.improve(parsed, g_config.llm_timeout_ms))
                            parsed.confidence = g_scorer.score(parsed);
                        metrics.recordLLMFallback(llm_t.elapsedMs());
                    }

                    if (g_config.cache_enabled && g_cache &&
                        parsed.confidence >= g_config.cache_min_confidence)
                        g_cache->put(sanitized, parsed);
                }
            }

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
            for (const auto& k : out_rec.getMemberNames())
                if (!csv_seen.count(k)) { csv_seen.insert(k); csv_keys.push_back(k); }
        }
    }

    double latency = total_timer.elapsedMs();
    std::cout << "[GeoEnrichLMDB] ✔ POST /api/v1/enrich/geo/lmdb"
              << " | records_in="  << process_N
              << " | succeeded="   << succeeded
              << " | failed="      << failed
              << " | geo_applied=" << geo_applied_count
              << " | latency="     << latency << "ms\n";

    LOG_F(INFO, "[enrich/geo/lmdb] records=%d succeeded=%d failed=%d geo_applied=%d latency=%.2fms",
          process_N, succeeded, failed, geo_applied_count, latency);
    if (failed > 0)
        LOG_F(WARNING, "[enrich/geo/lmdb] %d record(s) failed validation", failed);

    Json::Value resp;
    resp["total"]             = process_N;
    resp["succeeded"]         = succeeded;
    resp["failed"]            = failed;
    resp["geo_applied_count"] = geo_applied_count;
    resp["geo_backend"]       = "lmdb";
    resp["geo_db_ready"]      = geo.isReady();
    resp["address_column"]    = addr_col;
    resp["key_column"]        = key_col.empty()
                                ? Json::Value(Json::nullValue) : Json::Value(key_col);
    resp["normalize_columns"] = norm_spec;
    resp["total_latency_ms"]  = latency;
    resp["results"]           = output;

    if (truncated) {
        resp["warning"] = "Received " + std::to_string(total_received) +
                          " records but maximum is " + std::to_string(g_config.batch_max_size) +
                          ". Only first " + std::to_string(process_N) +
                          " records processed. Remaining " +
                          std::to_string(total_received - process_N) +
                          " records were discarded.";
        resp["total_received"]  = total_received;
        resp["total_processed"] = process_N;
        resp["total_discarded"] = total_received - process_N;
    }

    metrics.recordBatch(process_N, latency);
    if constexpr (WRITE_JSON_DEBUG) writeJsonDebug(output);
    if constexpr (WRITE_CSV_DEBUG)  writeCsvDebug(output, csv_keys);

    auto r = drogon::HttpResponse::newHttpJsonResponse(resp);
    callback(r);
}

} // namespace addr