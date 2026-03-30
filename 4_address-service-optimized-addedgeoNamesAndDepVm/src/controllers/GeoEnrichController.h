#pragma once

#include <drogon/HttpController.h>
#include <unordered_set>
#include <string>
#include "controllers/AuthFilter.h"
#include "models/AddressModels.h"

namespace addr {

// =============================================================================
//  GeoEnrichController — POST /api/v1/enrich/geo
//
//  Extends /api/v1/enrich with Phase 2 geo lookup:
//    Phase 2A: postal_code → city + state  (allCountries postal DB)
//    Phase 2B: city name   → state         (cities15000 global DB)
//
//  Fill strategy:
//    - Always fill blank fields
//    - Override existing field if libpostal confidence < 0.7
//
//  Supported countries: IN (India), US (United States), GB (United Kingdom)
//
//  Request body — same as /api/v1/enrich:
//  {
//    "data": [...],
//    "metadata": {
//      "address_column":    "addr",
//      "key_column":        "id",
//      "normalize_columns": "ALL",
//      "language":          "en",
//      "country":           "in"
//    }
//  }
//
//  Response — same as /api/v1/enrich plus extra fields per record:
//    "geo_city"        — city from geo lookup (if applied)
//    "geo_state"       — state from geo lookup (if applied)
//    "geo_source"      — "postal_2a" | "city_2b" | "none"
//    "geo_applied"     — true if geo lookup changed any field
// =============================================================================
class GeoEnrichController : public drogon::HttpController<GeoEnrichController> {
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(GeoEnrichController::geoEnrich,
                  "/api/v1/enrich/geo", drogon::Post, "addr::AuthFilter");
    METHOD_LIST_END

    void geoEnrich(const drogon::HttpRequestPtr& req,
                   std::function<void(const drogon::HttpResponsePtr&)>&& callback);

    static constexpr bool WRITE_JSON_DEBUG = false;
    static constexpr bool WRITE_CSV_DEBUG  = false;

private:
    static constexpr int MAX_EXPANSIONS = 3;
    static constexpr int MAX_RECORDS    = 5000;

    // Resolve ISO country code from parsed address or request hint
    static std::string resolveCountryCode(const ParsedAddress& parsed,
                                           const std::string&   hint);

    // Apply Phase 2 geo lookup to a parsed address
    // Returns true if any field was changed
    static bool applyGeoLookup(ParsedAddress&     parsed,
                                double             confidence,
                                const std::string& country_code,
                                std::string&       geo_source);

    // Shared helpers (same as EnrichController)
    static std::string detectAddressColumn(const Json::Value& first_record);
    static std::string detectKeyColumn(const Json::Value& first_record,
                                       const std::string& addr_col);
    static std::unordered_set<std::string> parseNormalizeColumns(const std::string& spec);
    static Json::Value buildOutputRecord(
        const Json::Value&                      raw_record,
        const ParsedAddress&                    parsed,
        const std::vector<std::string>&         expansions,
        const std::unordered_set<std::string>&  normalize_cols,
        const std::string&                      addr_col,
        const std::string&                      geo_source,
        bool                                    geo_applied);

    static void writeJsonDebug(const Json::Value& results);
    static void writeCsvDebug(const Json::Value& results,
                              const std::vector<std::string>& all_keys);
};

} // namespace addr