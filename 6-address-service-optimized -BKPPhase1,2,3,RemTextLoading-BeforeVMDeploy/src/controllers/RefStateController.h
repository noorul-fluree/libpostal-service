#pragma once

#include <drogon/HttpController.h>
#include "controllers/AuthFilter.h"

namespace addr {

// =============================================================================
//  RefStateController
//
//  GET /ref/v1/state/{country}/{abbrev}
//
//  Examples:
//    GET /ref/v1/state/IN/19    → "karnataka"
//    GET /ref/v1/state/US/CA    → "california"
//    GET /ref/v1/state/GB/ENG   → "england"
//
//  Response:
//  {
//    "country_code":  "IN",
//    "admin1_code":   "19",
//    "state_name":    "karnataka",
//    "found":         true
//  }
// =============================================================================
class RefStateController
    : public drogon::HttpController<RefStateController> {
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(RefStateController::getByCode,
                  "/ref/v1/state/{country}/{abbrev}",
                  drogon::Get, "addr::AuthFilter");
    METHOD_LIST_END

    void getByCode(const drogon::HttpRequestPtr& req,
                   std::function<void(const drogon::HttpResponsePtr&)>&& cb,
                   std::string country,
                   std::string abbrev);
};

} // namespace addr