#include "controllers/RefCityController.h"
#include "services/GeoNamesLMDB.h"
#include "utils/Logger.h"
#include <algorithm>
#include <cctype>

namespace addr {

static std::string normalizeLower(const std::string& s) {
    std::string r = s;
    std::transform(r.begin(), r.end(), r.begin(),
        [](unsigned char c){ return std::tolower(c); });
    return r;
}

void RefCityController::search(
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

    std::string query   = req->getParameter("q");
    std::string country = req->getParameter("country");

    if (query.empty()) {
        Json::Value err; err["error"] = "query parameter 'q' is required";
        auto r = drogon::HttpResponse::newHttpJsonResponse(err);
        r->setStatusCode(drogon::k400BadRequest);
        cb(r); return;
    }

    std::transform(country.begin(), country.end(), country.begin(), ::toupper);
    std::string q_lower = normalizeLower(query);

    LOG_F(INFO, "[ref/city/search] q=%s country=%s",
          query.c_str(), country.empty() ? "(any)" : country.c_str());

    Json::Value result;
    result["query"]        = query;
    result["country_code"] = country.empty() ? Json::Value(Json::nullValue)
                                              : Json::Value(country);

    auto doLookup = [&](const std::string& cc) -> bool {
        // Step 1: check alias (bombay → mumbai)
        std::string canonical = geo.lookupAlias(q_lower, cc);
        bool alias_resolved = !canonical.empty();
        std::string lookup_name = alias_resolved ? canonical : q_lower;

        // Step 2: look up state
        std::string state = geo.lookupCityState(lookup_name, cc);
        if (state.empty() && alias_resolved) {
            // try original name too
            state = geo.lookupCityState(q_lower, cc);
            if (!state.empty()) {
                canonical = q_lower;
                alias_resolved = false;
            }
        }

        if (!state.empty() || alias_resolved) {
            result["canonical_name"]  = canonical.empty() ? q_lower : canonical;
            result["state"]           = state.empty()
                                        ? Json::Value(Json::nullValue)
                                        : Json::Value(state);
            result["alias_resolved"]  = alias_resolved;
            result["found"]           = true;
            result["country_code"]    = cc;
            return true;
        }
        return false;
    };

    bool found = false;
    if (!country.empty()) {
        found = doLookup(country);
    } else {
        // Try most common countries
        static const std::vector<std::string> kTryOrder = {
            "IN","US","GB","CN","JP","DE","FR","AU","CA","BR",
            "IT","ES","MX","KR","TH","SG","MY","PH","ID","PK"
        };
        for (const auto& cc : kTryOrder) {
            if (doLookup(cc)) { found = true; break; }
        }
    }

    if (!found) {
        result["canonical_name"] = Json::Value(Json::nullValue);
        result["state"]          = Json::Value(Json::nullValue);
        result["alias_resolved"] = false;
        result["found"]          = false;
    }

    cb(drogon::HttpResponse::newHttpJsonResponse(result));
}

} // namespace addr