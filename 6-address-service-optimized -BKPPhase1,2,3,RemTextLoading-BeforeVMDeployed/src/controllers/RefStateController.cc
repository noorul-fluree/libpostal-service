#include "controllers/RefStateController.h"
#include "services/GeoNamesLMDB.h"
#include "utils/Logger.h"

namespace addr {

void RefStateController::getByCode(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& cb,
    std::string country,
    std::string abbrev)
{
    auto& geo = GeoNamesLMDB::instance();
    if (!geo.isReady()) {
        Json::Value err; err["error"] = "GeoNames LMDB not initialized";
        auto r = drogon::HttpResponse::newHttpJsonResponse(err);
        r->setStatusCode(drogon::k503ServiceUnavailable);
        cb(r); return;
    }

    if (country.empty() || abbrev.empty()) {
        Json::Value err; err["error"] = "country and abbrev are required";
        auto r = drogon::HttpResponse::newHttpJsonResponse(err);
        r->setStatusCode(drogon::k400BadRequest);
        cb(r); return;
    }

    // Normalize — country code uppercase, abbrev as-is
    std::transform(country.begin(), country.end(), country.begin(), ::toupper);

    LOG_F(INFO, "[ref/state] country=%s abbrev=%s",
          country.c_str(), abbrev.c_str());

    std::string state = geo.lookupState(country, abbrev);
    Json::Value result;
    result["country_code"] = country;
    result["admin1_code"]  = abbrev;

    if (!state.empty()) {
        result["state_name"] = state;
        result["found"]      = true;
    } else {
        result["state_name"] = Json::Value(Json::nullValue);
        result["found"]      = false;
    }

    cb(drogon::HttpResponse::newHttpJsonResponse(result));
}

} // namespace addr