#pragma once

#include <drogon/HttpController.h>
#include "controllers/AuthFilter.h"

namespace addr {

// =============================================================================
//  RefCityController
//
//  GET /ref/v1/city/search?q=bombay&country=IN
//
//  Query params:
//    q       — city name (required)
//    country — ISO alpha2 country code (optional, narrows results)
//
//  Resolves:
//    1. Checks alias table (bombay → mumbai)
//    2. Looks up state for canonical name
//
//  Response:
//  {
//    "query":          "bombay",
//    "country_code":   "IN",
//    "canonical_name": "mumbai",
//    "state":          "maharashtra",
//    "alias_resolved": true,
//    "found":          true
//  }
// =============================================================================
class RefCityController
    : public drogon::HttpController<RefCityController> {
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(RefCityController::search,
                  "/ref/v1/city/search",
                  drogon::Get, "addr::AuthFilter");
    METHOD_LIST_END

    void search(const drogon::HttpRequestPtr& req,
                std::function<void(const drogon::HttpResponsePtr&)>&& cb);
};

} // namespace addr