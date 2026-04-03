#pragma once

#include <drogon/HttpController.h>
#include "controllers/AuthFilter.h"

namespace addr {

// =============================================================================
//  RefCountryController
//
//  GET /ref/v1/country/{code_or_name}
//
//  Accepts: alpha2 (IN), alpha3 (IND), numeric (356), or name (india)
//
//  Response:
//  {
//    "alpha2":      "IN",
//    "alpha3":      "IND",
//    "numeric_code":"356",
//    "name":        "india",
//    "region":      "asia",
//    "sub_region":  "southern asia",
//    "found":       true
//  }
// =============================================================================
class RefCountryController
    : public drogon::HttpController<RefCountryController> {
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(RefCountryController::getByCode,
                  "/ref/v1/country/{code}",
                  drogon::Get, "addr::AuthFilter");
    METHOD_LIST_END

    void getByCode(const drogon::HttpRequestPtr& req,
                   std::function<void(const drogon::HttpResponsePtr&)>&& cb,
                   std::string code);
};

} // namespace addr