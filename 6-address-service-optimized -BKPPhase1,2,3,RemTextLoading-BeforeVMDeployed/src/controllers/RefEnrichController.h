#pragma once

#include <drogon/HttpController.h>
#include "controllers/AuthFilter.h"

namespace addr {

// =============================================================================
//  RefEnrichController
//
//  POST /ref/v1/enrich   — fill missing fields using reference data
//  POST /ref/v1/validate — validate parsed address against reference data
//
//  Both accept a parsed address object and return enriched/validated result.
//
//  /ref/v1/enrich request:
//  {
//    "house_number": "123",
//    "road":         "mg road",
//    "city":         "bengaluru",
//    "state":        "",
//    "postcode":     "560001",
//    "country":      "india",
//    "country_code": "IN"          ← optional hint
//  }
//
//  /ref/v1/enrich response:
//  {
//    "house_number": "123",
//    "road":         "mg road",
//    "city":         "bengaluru",
//    "state":        "karnataka",   ← filled from reference data
//    "postcode":     "560001",
//    "country":      "india",
//    "country_code": "IN",
//    "enriched_fields": ["state"],  ← which fields were filled
//    "geo_source":   "postal_2a",
//    "confidence_boost": 0.12       ← how much confidence improved
//  }
//
//  /ref/v1/validate response adds:
//  {
//    ...enriched fields...,
//    "valid":              true,
//    "conflicts":          [],       ← field conflicts found
//    "validation_notes":   ["postcode matches city and state"]
//  }
// =============================================================================
class RefEnrichController
    : public drogon::HttpController<RefEnrichController> {
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(RefEnrichController::enrich,
                  "/ref/v1/enrich",
                  drogon::Post, "addr::AuthFilter");
    ADD_METHOD_TO(RefEnrichController::validate,
                  "/ref/v1/validate",
                  drogon::Post, "addr::AuthFilter");
    METHOD_LIST_END

    void enrich(const drogon::HttpRequestPtr& req,
                std::function<void(const drogon::HttpResponsePtr&)>&& cb);

    void validate(const drogon::HttpRequestPtr& req,
                  std::function<void(const drogon::HttpResponsePtr&)>&& cb);

private:
    struct EnrichResult {
        std::string city;
        std::string state;
        std::string country;
        std::string country_code;
        std::string geo_source;
        std::vector<std::string> enriched_fields;
        std::vector<std::string> conflicts;
        std::vector<std::string> validation_notes;
        double confidence_boost = 0.0;
    };

    static std::string resolveCountryCode(const Json::Value& body);
    static EnrichResult applyEnrichment(const Json::Value& body,
                                         const std::string& cc);
    static Json::Value buildResponse(const Json::Value& body,
                                      const EnrichResult& result,
                                      bool include_validation);
};

} // namespace addr