#include "controllers/HealthController.h"
#include "services/AddressParser.h"
#include "services/CacheManager.h"
#include "services/MetricsCollector.h"
#include "models/AddressModels.h"

#include <chrono>

extern addr::AddressParser   g_parser;
extern addr::CacheManager*   g_cache;    // pointer
extern addr::ServiceConfig   g_config;
extern std::atomic<bool>     g_service_ready;

namespace addr {

void HealthController::live(const drogon::HttpRequestPtr&,
                             std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
    Json::Value j;
    j["status"] = "alive";
    j["timestamp"] = static_cast<Json::Int64>(
        std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()));
    callback(drogon::HttpResponse::newHttpJsonResponse(j));
}

void HealthController::ready(const drogon::HttpRequestPtr&,
                              std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
    Json::Value j;
    bool parser_ready  = g_parser.isReady();
    bool overall_ready = parser_ready && g_service_ready.load();

    j["status"]               = overall_ready ? "ready" : "not_ready";
    j["checks"]["libpostal"]  = parser_ready ? "ok" : "not_initialized";
    j["checks"]["cache"]      = g_config.cache_enabled ? "enabled" : "disabled";
    j["checks"]["llm"]        = "disabled";

    auto resp = drogon::HttpResponse::newHttpJsonResponse(j);
    if (!overall_ready) resp->setStatusCode(drogon::k503ServiceUnavailable);
    callback(resp);
}

void HealthController::startup(const drogon::HttpRequestPtr&,
                                std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
    Json::Value j;
    bool ready = g_parser.isReady();
    j["status"]           = ready ? "started" : "starting";
    j["libpostal_loaded"] = ready;

    auto resp = drogon::HttpResponse::newHttpJsonResponse(j);
    if (!ready) resp->setStatusCode(drogon::k503ServiceUnavailable);
    callback(resp);
}

void HealthController::info(const drogon::HttpRequestPtr&,
                             std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
    Json::Value j;
    auto& metrics = MetricsCollector::instance();

    j["version"] = "1.0.0";
    j["service"] = "address-normalization-service";

    j["config"]["port"]              = g_config.port;
    j["config"]["threads"]           = g_config.threads;
    j["config"]["cache_enabled"]     = g_config.cache_enabled;
    j["config"]["cache_max_entries"] = static_cast<Json::UInt64>(g_config.cache_max_entries);
    j["config"]["llm_enabled"]       = false;
    j["config"]["batch_max_size"]    = g_config.batch_max_size;

    j["stats"]["total_requests"]           = static_cast<Json::UInt64>(metrics.total_requests.load());
    j["stats"]["total_addresses_processed"]= static_cast<Json::UInt64>(metrics.total_addresses_processed.load());
    j["stats"]["parse_success"]            = static_cast<Json::UInt64>(metrics.total_parse_success.load());
    j["stats"]["parse_errors"]             = static_cast<Json::UInt64>(metrics.total_parse_errors.load());
    j["stats"]["batch_requests"]           = static_cast<Json::UInt64>(metrics.total_batch_requests.load());

    if (g_cache) {
        j["cache"]["size"]      = static_cast<Json::UInt64>(g_cache->size());
        j["cache"]["hits"]      = static_cast<Json::UInt64>(g_cache->hits());
        j["cache"]["misses"]    = static_cast<Json::UInt64>(g_cache->misses());
        j["cache"]["hit_ratio"] = g_cache->hitRatio();
    }

    j["pipeline"]["phase1"] = static_cast<Json::UInt64>(metrics.phase1_count.load());
    j["pipeline"]["phase2"] = static_cast<Json::UInt64>(metrics.phase2_count.load());
    j["pipeline"]["phase3"] = static_cast<Json::UInt64>(metrics.phase3_count.load());

    callback(drogon::HttpResponse::newHttpJsonResponse(j));
}

} // namespace addr