#include "controllers/RefPostalController.h"
#include "services/GeoNamesLMDB.h"
#include "utils/Logger.h"
#include <algorithm>

namespace addr {

// =============================================================================
//  buildResult — lookup postal code and build JSON response
// =============================================================================
Json::Value RefPostalController::buildResult(const std::string& postal_code,
                                              const std::string& country_code) {
    Json::Value j;
    j["postal_code"]   = postal_code;
    j["country_code"]  = country_code;

    auto& geo = GeoNamesLMDB::instance();
    auto entry = geo.lookupPostalFull(postal_code, country_code);

    if (entry.has_value()) {
        j["city"]    = entry->city;
        j["state"]   = entry->state;
        j["found"]   = true;
    } else {
        j["city"]    = Json::Value(Json::nullValue);
        j["state"]   = Json::Value(Json::nullValue);
        j["found"]   = false;
    }
    return j;
}

// =============================================================================
//  getByCode — GET /ref/v1/postal/{code}?country=XX
// =============================================================================
void RefPostalController::getByCode(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& cb,
    std::string code)
{
    auto& geo = GeoNamesLMDB::instance();
    if (!geo.isReady()) {
        Json::Value err;
        err["error"] = "GeoNames LMDB not initialized";
        auto r = drogon::HttpResponse::newHttpJsonResponse(err);
        r->setStatusCode(drogon::k503ServiceUnavailable);
        cb(r); return;
    }

    if (code.empty()) {
        Json::Value err; err["error"] = "postal code is required";
        auto r = drogon::HttpResponse::newHttpJsonResponse(err);
        r->setStatusCode(drogon::k400BadRequest);
        cb(r); return;
    }

    // country from query param — optional
    std::string country = req->getParameter("country");
    std::transform(country.begin(), country.end(), country.begin(), ::toupper);

    LOG_F(INFO, "[ref/postal] code=%s country=%s",
          code.c_str(), country.empty() ? "(any)" : country.c_str());

    Json::Value result;

    if (!country.empty()) {
        // Scoped lookup
        result = buildResult(code, country);
    } else {
        // Try common countries in order of global postal volume
        static const std::vector<std::string> kTryOrder = {
            "US","GB","DE","FR","CA","AU","IN","JP","CN","BR",
            "IT","ES","NL","PL","SE","NO","DK","FI","AT","CH",
            "BE","PT","MX","AR","ZA","KR","TH","SG","MY","PH"
        };
        bool found = false;
        for (const auto& cc : kTryOrder) {
            auto entry = geo.lookupPostalFull(code, cc);
            if (entry.has_value()) {
                result = buildResult(code, cc);
                found = true;
                break;
            }
        }
        if (!found) {
            result["postal_code"]  = code;
            result["country_code"] = Json::Value(Json::nullValue);
            result["city"]         = Json::Value(Json::nullValue);
            result["state"]        = Json::Value(Json::nullValue);
            result["found"]        = false;
        }
    }

    cb(drogon::HttpResponse::newHttpJsonResponse(result));
}

// =============================================================================
//  batchLookup — POST /ref/v1/postal/batch
//
//  Request body:
//  {
//    "lookups": [
//      {"postal_code": "560001", "country_code": "IN"},
//      {"postal_code": "10001",  "country_code": "US"},
//      {"postal_code": "SW1A"}
//    ]
//  }
// =============================================================================
void RefPostalController::batchLookup(
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
    if (!json || !json->isMember("lookups") || !(*json)["lookups"].isArray()) {
        Json::Value err; err["error"] = "Missing 'lookups' array";
        auto r = drogon::HttpResponse::newHttpJsonResponse(err);
        r->setStatusCode(drogon::k400BadRequest);
        cb(r); return;
    }

    const Json::Value& lookups = (*json)["lookups"];
    int N = static_cast<int>(lookups.size());
    if (N == 0) {
        Json::Value err; err["error"] = "Empty lookups array";
        auto r = drogon::HttpResponse::newHttpJsonResponse(err);
        r->setStatusCode(drogon::k400BadRequest);
        cb(r); return;
    }
    if (N > MAX_BATCH) N = MAX_BATCH; // silently truncate

    Json::Value results(Json::arrayValue);
    int found_count = 0;

    for (int i = 0; i < N; ++i) {
        const Json::Value& item = lookups[i];
        std::string postal  = item.get("postal_code",  "").asString();
        std::string country = item.get("country_code", "").asString();
        std::transform(country.begin(), country.end(), country.begin(), ::toupper);

        if (postal.empty()) {
            Json::Value r;
            r["postal_code"]  = postal;
            r["country_code"] = country;
            r["found"]        = false;
            r["error"]        = "postal_code is required";
            results.append(r);
            continue;
        }

        Json::Value r = buildResult(postal, country);
        if (r["found"].asBool()) ++found_count;
        results.append(r);
    }

    LOG_F(INFO, "[ref/postal/batch] total=%d found=%d not_found=%d",
          N, found_count, N - found_count);

    Json::Value resp;
    resp["total"]     = N;
    resp["found"]     = found_count;
    resp["not_found"] = N - found_count;
    resp["results"]   = results;

    cb(drogon::HttpResponse::newHttpJsonResponse(resp));
}

void RefPostalController::reverseByCity(
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

    std::string city = req->getParameter("city");
    std::string cc   = req->getParameter("country");

    if (city.empty()) {
        Json::Value err; err["error"] = "Missing required parameter: city";
        auto r = drogon::HttpResponse::newHttpJsonResponse(err);
        r->setStatusCode(drogon::k400BadRequest);
        cb(r); return;
    }

    // Auto-detect country if not provided — try all major ones
    std::vector<std::string> countries_to_try;
    if (!cc.empty()) {
        std::transform(cc.begin(), cc.end(), cc.begin(), ::toupper);
        countries_to_try.push_back(cc);
    } else {
        countries_to_try = {"IN","US","GB","DE","FR","AU","CA","JP","CN","SG"};
    }

    Json::Value out;
    out["city"] = city;

    for (const auto& try_cc : countries_to_try) {
        auto result = geo.lookupPostalReverse(city, try_cc);
        if (!result.postcodes.empty()) {
            out["found"]        = true;
            out["country_code"] = try_cc;
            out["total_stored"] = result.total_stored;
            out["returned"]     = (int)result.postcodes.size();
            out["note"]         = result.total_stored == 50
                ? "database stores max 50 codes per city; true count may be higher"
                : "";
            Json::Value arr(Json::arrayValue);
            for (const auto& pc : result.postcodes) arr.append(pc);
            out["postal_codes"] = arr;

            LOG_F(INFO, "[ref/postal/reverse] city=%s cc=%s found=%d",
                  city.c_str(), try_cc.c_str(), (int)result.postcodes.size());
            cb(drogon::HttpResponse::newHttpJsonResponse(out));
            return;
        }
    }

    out["found"] = false;
    LOG_F(INFO, "[ref/postal/reverse] city=%s not found", city.c_str());
    cb(drogon::HttpResponse::newHttpJsonResponse(out));
}

} // namespace addr