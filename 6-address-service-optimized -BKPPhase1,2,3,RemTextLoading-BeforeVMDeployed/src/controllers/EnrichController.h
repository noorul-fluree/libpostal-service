#pragma once

#include <drogon/HttpController.h>
#include <unordered_set>
#include <string>
#include "controllers/AuthFilter.h"
#include "models/AddressModels.h"

namespace addr {

class EnrichController : public drogon::HttpController<EnrichController> {
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(EnrichController::enrich, "/api/v1/enrich", drogon::Post, "addr::AuthFilter");
    METHOD_LIST_END

    void enrich(const drogon::HttpRequestPtr& req,
                std::function<void(const drogon::HttpResponsePtr&)>&& callback);

    // -------------------------------------------------------------------------
    //  Compile-time debug file flags
    //  Change false → true and rebuild to enable debug output.
    //
    //  Both false  = production mode (HTTP response only, zero file I/O)
    //  WRITE_JSON  = writes ./uploads/debug_results.json after each request
    //  WRITE_CSV   = writes ./uploads/debug_results.csv  after each request
    // -------------------------------------------------------------------------
    static constexpr bool WRITE_JSON_DEBUG = false; // true
    static constexpr bool WRITE_CSV_DEBUG  = false; // true

private:
    static constexpr int MAX_EXPANSIONS = 3;

    static std::string detectAddressColumn(const Json::Value& first_record);
    static std::string detectKeyColumn(const Json::Value& first_record,
                                       const std::string& addr_col);
    static std::unordered_set<std::string> parseNormalizeColumns(const std::string& spec);

    static Json::Value buildOutputRecord(
        const Json::Value&                      raw_record,
        const ParsedAddress&                    parsed,
        const std::vector<std::string>&         expansions,
        const std::unordered_set<std::string>&  normalize_cols,
        const std::string&                      addr_col);

    static void writeJsonDebug(const Json::Value& results);
    static void writeCsvDebug(const Json::Value& results,
                              const std::vector<std::string>& all_keys);
};

} // namespace addr