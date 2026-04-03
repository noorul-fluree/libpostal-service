#include "controllers/RefCountryController.h"
#include "services/GeoNamesLMDB.h"
#include "utils/Logger.h"

namespace addr {

void RefCountryController::getByCode(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& cb,
    std::string code)
{
    auto& geo = GeoNamesLMDB::instance();
    if (!geo.isReady()) {
        Json::Value err; err["error"] = "GeoNames LMDB not initialized";
        auto r = drogon::HttpResponse::newHttpJsonResponse(err);
        r->setStatusCode(drogon::k503ServiceUnavailable);
        cb(r); return;
    }

    if (code.empty()) {
        Json::Value err; err["error"] = "country code or name is required";
        auto r = drogon::HttpResponse::newHttpJsonResponse(err);
        r->setStatusCode(drogon::k400BadRequest);
        cb(r); return;
    }

    LOG_F(INFO, "[ref/country] query=%s", code.c_str());

    auto record = geo.lookupCountry(code);
    Json::Value result;

    if (record.has_value()) {
        result["alpha2"]       = record->alpha2;
        result["alpha3"]       = record->alpha3;
        result["numeric_code"] = record->numeric_code;
        result["name"]         = record->name;
        result["region"]       = record->region;
        result["sub_region"]   = record->sub_region;
        result["found"]        = true;
    } else {
        result["query"] = code;
        result["found"] = false;
    }

    cb(drogon::HttpResponse::newHttpJsonResponse(result));
}

} // namespace addr