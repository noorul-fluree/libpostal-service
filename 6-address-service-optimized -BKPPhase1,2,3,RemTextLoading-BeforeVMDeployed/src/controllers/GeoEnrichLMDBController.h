#pragma once

#include <drogon/HttpController.h>
#include <unordered_set>
#include <string>
#include "controllers/AuthFilter.h"
#include "models/AddressModels.h"

namespace addr {

// =============================================================================
//  GeoEnrichLMDBController — POST /api/v1/enrich/geo/lmdb
//
//  Same pipeline as /api/v1/enrich/geo but uses LMDB for geo lookup:
//    - ~5MB RAM vs ~307MB for in-memory approach
//    - <1s startup vs ~30s
//    - O(log n) B-tree lookup, ~10-50 microseconds per record
//
//  /api/v1/enrich/geo (in-memory) stays untouched as backup.
//
//  Request body — identical to /api/v1/enrich/geo:
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
//  Response — same as /api/v1/enrich/geo plus:
//    "geo_backend": "lmdb"   — confirms LMDB path was used
// =============================================================================
class GeoEnrichLMDBController
    : public drogon::HttpController<GeoEnrichLMDBController> {
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(GeoEnrichLMDBController::geoEnrichLMDB,
                  "/api/v1/enrich/geo/lmdb", drogon::Post, "addr::AuthFilter");
    METHOD_LIST_END

    void geoEnrichLMDB(const drogon::HttpRequestPtr& req,
                       std::function<void(const drogon::HttpResponsePtr&)>&& callback);

    static constexpr bool WRITE_JSON_DEBUG = false;
    static constexpr bool WRITE_CSV_DEBUG  = false;

private:
    static constexpr int MAX_EXPANSIONS = 3;

    static std::string resolveCountryCode(const ParsedAddress& parsed,
                                           const std::string&   hint);
    static bool applyGeoLookup(ParsedAddress&     parsed,
                                double             confidence,
                                const std::string& country_code,
                                std::string&       geo_source);

    static std::string detectAddressColumn(const Json::Value& first_record);
    static std::string detectKeyColumn(const Json::Value& first_record,
                                       const std::string& addr_col);
    static std::unordered_set<std::string> parseNormalizeColumns(
        const std::string& spec);

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