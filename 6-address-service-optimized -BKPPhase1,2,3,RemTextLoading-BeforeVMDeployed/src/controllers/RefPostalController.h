#pragma once

#include <drogon/HttpController.h>
#include "controllers/AuthFilter.h"

namespace addr {

// =============================================================================
//  RefPostalController
//
//  GET  /ref/v1/postal/{code}               — lookup by postal code (auto-detect country)
//  GET  /ref/v1/postal/{code}?country=IN    — country-scoped lookup
//  POST /ref/v1/postal/batch                — batch postal lookup
//
//  Response (single):
//  {
//    "postal_code": "560001",
//    "country_code": "IN",
//    "city": "bengaluru",
//    "state": "karnataka",
//    "found": true
//  }
//
//  Response (batch):
//  {
//    "total": 3,
//    "found": 2,
//    "not_found": 1,
//    "results": [ { "postal_code": "...", "country_code": "...", ... } ]
//  }
// =============================================================================
class RefPostalController
    : public drogon::HttpController<RefPostalController> {
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(RefPostalController::getByCode,
                "/ref/v1/postal/{code}",
                drogon::Get, "addr::AuthFilter");
    ADD_METHOD_TO(RefPostalController::batchLookup,
                "/ref/v1/postal/batch",
                drogon::Post, "addr::AuthFilter");
    ADD_METHOD_TO(RefPostalController::reverseByCity,
                "/ref/v1/postal/reverse",
                drogon::Get, "addr::AuthFilter");
    METHOD_LIST_END

    // GET /ref/v1/postal/{code}?country=XX
    void getByCode(const drogon::HttpRequestPtr& req,
                   std::function<void(const drogon::HttpResponsePtr&)>&& cb,
                   std::string code);

    // POST /ref/v1/postal/batch
    void batchLookup(const drogon::HttpRequestPtr& req,
                     std::function<void(const drogon::HttpResponsePtr&)>&& cb);

    // GET /ref/v1/postal/reverse?city=XXX&country=XX
    void reverseByCity(const drogon::HttpRequestPtr& req,
                   std::function<void(const drogon::HttpResponsePtr&)>&& cb);

private:
    static constexpr int MAX_BATCH = 1000;

    static Json::Value buildResult(const std::string& postal_code,
                                   const std::string& country_code);
};

} // namespace addr