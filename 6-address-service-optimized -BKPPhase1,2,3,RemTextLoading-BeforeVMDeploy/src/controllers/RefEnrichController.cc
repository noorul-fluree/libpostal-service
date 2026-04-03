#include "controllers/RefEnrichController.h"
#include "services/GeoNamesLMDB.h"
#include "utils/Logger.h"
#include <algorithm>
#include <cctype>

namespace addr {

// =============================================================================
//  helpers
// =============================================================================
static std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
        [](unsigned char c){ return std::tolower(c); });
    return s;
}

static std::string toUpper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), ::toupper);
    return s;
}

static std::string getStr(const Json::Value& j, const char* key) {
    if (j.isMember(key) && j[key].isString()) return j[key].asString();
    return {};
}

// =============================================================================
//  resolveCountryCode
//  Priority: explicit country_code field → country name → postcode pattern
// =============================================================================
std::string RefEnrichController::resolveCountryCode(const Json::Value& body) {
    // Explicit 2-letter code
    std::string cc = toUpper(getStr(body, "country_code"));
    if (cc.size() == 2) return cc;

    // Try country name via LMDB
    std::string cname = toLower(getStr(body, "country"));
    if (!cname.empty()) {
        auto rec = GeoNamesLMDB::instance().lookupCountry(cname);
        if (rec.has_value() && !rec->alpha2.empty()) return rec->alpha2;
    }

    // UK postcode auto-detect
    std::string pc = getStr(body, "postcode");
    if (!pc.empty() && std::isalpha(static_cast<unsigned char>(pc[0])))
        return "GB";

    return {};
}

// =============================================================================
//  applyEnrichment — core logic used by both enrich and validate
// =============================================================================
RefEnrichController::EnrichResult
RefEnrichController::applyEnrichment(const Json::Value& body,
                                      const std::string& cc) {
    EnrichResult res;
    res.country_code = cc;
    auto& geo = GeoNamesLMDB::instance();

    std::string city     = toLower(getStr(body, "city"));
    std::string state    = toLower(getStr(body, "state"));
    std::string postcode = getStr(body, "postcode");
    std::string country  = toLower(getStr(body, "country"));

    // -----------------------------------------------------------------------
    //  Phase 2A: postal → city + state
    // -----------------------------------------------------------------------
    if (!postcode.empty()) {
        auto entry = geo.lookupPostal(postcode, cc);
        if (entry.has_value()) {
            res.geo_source = "postal_2a";

            if (city.empty() && !entry->city.empty()) {
                res.city = entry->city;
                res.enriched_fields.push_back("city");
                res.confidence_boost += 0.10;
            }
            if (state.empty() && !entry->state.empty()) {
                res.state = entry->state;
                res.enriched_fields.push_back("state");
                res.confidence_boost += 0.10;
            }

            // Resolve alias before conflict check
            std::string city_for_check = city;
            if (!city.empty()) {
                std::string alias = geo.lookupAlias(city, cc);
                if (!alias.empty()) city_for_check = alias;
            }
            // Cross-validate using canonical city name
// Note: postal data may store post office names (e.g. "tajmahal")
// not city names — only flag conflict if state also mismatches
           if (!city_for_check.empty() && !entry->city.empty() &&
                city_for_check != entry->city) {
                // Look up what state the given city actually belongs to
                std::string city_actual_state = geo.lookupCityStateFull(city_for_check, cc);
                bool state_matches = city_actual_state.empty() ||
                                    (city_actual_state == entry->state);

                if (!state_matches) {
                    res.conflicts.push_back(
                        "city '" + city_for_check + "' (state: " + city_actual_state +
                        ") does not match postal code '" + postcode +
                        "' (state: " + entry->state + ")");
                } else {
                    res.validation_notes.push_back(
                        "postal area name '" + entry->city +
                        "' differs from city '" + city_for_check +
                        "' but state matches - accepted");
                }
            }
            if (!state.empty() && !entry->state.empty() &&
                state != entry->state) {
                res.conflicts.push_back(
                    "state '" + state + "' does not match postal code '" +
                    postcode + "' → expected '" + entry->state + "'");
            }
        }
    }

    // -----------------------------------------------------------------------
    //  Phase 2B: city → state (if state still missing)
    // -----------------------------------------------------------------------
    std::string effective_city = res.city.empty() ? city : res.city;
    if (res.state.empty() && state.empty() && !effective_city.empty()) {
        std::string found_state = geo.lookupCityStateFull(effective_city, cc);
        if (!found_state.empty()) {
            res.state = found_state;
            res.enriched_fields.push_back("state");
            res.geo_source = res.geo_source.empty() ? "city_2b" : res.geo_source;
            res.confidence_boost += 0.08;
        }
    }

    // -----------------------------------------------------------------------
    //  Country fill
    // -----------------------------------------------------------------------
    if (country.empty() && !cc.empty()) {
        auto crec = geo.lookupCountry(cc);
        if (crec.has_value()) {
            res.country = crec->name;
            res.enriched_fields.push_back("country");
            res.confidence_boost += 0.05;
        }
    }

    // -----------------------------------------------------------------------
    //  Alias resolution for city
    // -----------------------------------------------------------------------
    if (!effective_city.empty()) {
        std::string canonical = geo.lookupAlias(effective_city, cc);
        if (!canonical.empty() && canonical != effective_city) {
            if (res.city.empty()) {
                res.city = canonical;
                res.enriched_fields.push_back("city_normalized");
            }
            res.validation_notes.push_back(
                "city alias resolved: '" + effective_city +
                "' → '" + canonical + "'");
        }
    }

    // -----------------------------------------------------------------------
    //  Validation notes
    // -----------------------------------------------------------------------
    if (res.conflicts.empty() && !postcode.empty() && res.geo_source == "postal_2a")
        res.validation_notes.push_back("postcode matches city and state");

    if (res.geo_source.empty())
        res.geo_source = "none";

    return res;
}

// =============================================================================
//  buildResponse
// =============================================================================
Json::Value RefEnrichController::buildResponse(const Json::Value& body,
                                                const EnrichResult& res,
                                                bool include_validation) {
    // Start with all input fields
    Json::Value out = body;

    // Apply enriched fields
    if (!res.city.empty())         out["city"]         = res.city;
    if (!res.state.empty())        out["state"]        = res.state;
    if (!res.country.empty())      out["country"]      = res.country;
    if (!res.country_code.empty()) out["country_code"] = res.country_code;

    out["geo_source"]       = res.geo_source;
    out["confidence_boost"] = res.confidence_boost;

    Json::Value ef(Json::arrayValue);
    for (const auto& f : res.enriched_fields) ef.append(f);
    out["enriched_fields"] = ef;

    if (include_validation) {
        out["valid"] = res.conflicts.empty();

        Json::Value conflicts(Json::arrayValue);
        for (const auto& c : res.conflicts) conflicts.append(c);
        out["conflicts"] = conflicts;

        Json::Value notes(Json::arrayValue);
        for (const auto& n : res.validation_notes) notes.append(n);
        out["validation_notes"] = notes;
    }

    return out;
}

// =============================================================================
//  enrich — POST /ref/v1/enrich
// =============================================================================
void RefEnrichController::enrich(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& cb)
{
    auto& geo = GeoNamesLMDB::instance();
    if (!geo.isReady()) {
        Json::Value err; err["error"] = "GeoNames LMDB not initialized";
        auto r = drogon::HttpResponse::newHttpJsonResponse(err);
        r->setStatusCode(drogon::k503ServiceUnavailable);
        cb(r); return;
    }

    auto json = req->getJsonObject();
    if (!json) {
        Json::Value err; err["error"] = "Invalid JSON body";
        auto r = drogon::HttpResponse::newHttpJsonResponse(err);
        r->setStatusCode(drogon::k400BadRequest);
        cb(r); return;
    }

    std::string cc = resolveCountryCode(*json);
    LOG_F(INFO, "[ref/enrich] postcode=%s city=%s cc=%s",
          getStr(*json, "postcode").c_str(),
          getStr(*json, "city").c_str(),
          cc.empty() ? "(unknown)" : cc.c_str());

    auto result = applyEnrichment(*json, cc);
    cb(drogon::HttpResponse::newHttpJsonResponse(
        buildResponse(*json, result, false)));
}

// =============================================================================
//  validate — POST /ref/v1/validate
// =============================================================================
void RefEnrichController::validate(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& cb)
{
    auto& geo = GeoNamesLMDB::instance();
    if (!geo.isReady()) {
        Json::Value err; err["error"] = "GeoNames LMDB not initialized";
        auto r = drogon::HttpResponse::newHttpJsonResponse(err);
        r->setStatusCode(drogon::k503ServiceUnavailable);
        cb(r); return;
    }

    auto json = req->getJsonObject();
    if (!json) {
        Json::Value err; err["error"] = "Invalid JSON body";
        auto r = drogon::HttpResponse::newHttpJsonResponse(err);
        r->setStatusCode(drogon::k400BadRequest);
        cb(r); return;
    }

    std::string cc = resolveCountryCode(*json);
    LOG_F(INFO, "[ref/validate] postcode=%s city=%s cc=%s",
          getStr(*json, "postcode").c_str(),
          getStr(*json, "city").c_str(),
          cc.empty() ? "(unknown)" : cc.c_str());

    auto result = applyEnrichment(*json, cc);
    cb(drogon::HttpResponse::newHttpJsonResponse(
        buildResponse(*json, result, true)));
}

} // namespace addr