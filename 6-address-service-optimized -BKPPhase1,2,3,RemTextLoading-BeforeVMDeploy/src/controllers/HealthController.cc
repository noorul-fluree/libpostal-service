#include "utils/Logger.h"
#include "controllers/HealthController.h"
#include "services/AddressParser.h"
#include "services/CacheManager.h"
#include "services/LLMFallback.h"
#include "services/MetricsCollector.h"
#include "models/AddressModels.h"

#include <chrono>
#include <cstdlib>

extern addr::AddressParser  g_parser;
extern addr::CacheManager*  g_cache;
extern addr::LLMFallback    g_llm;
extern addr::ServiceConfig  g_config;
extern std::atomic<bool>    g_service_ready;

namespace addr {

void HealthController::live(const drogon::HttpRequestPtr& req,
                             std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
    Json::Value j;
    j["status"]    = "alive";
    j["timestamp"] = static_cast<Json::Int64>(
        std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()));
    callback(drogon::HttpResponse::newHttpJsonResponse(j));
}

void HealthController::ready(const drogon::HttpRequestPtr& req,
                              std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
    Json::Value j;

    bool parser_ok   = g_parser.isReady();
    bool overall     = parser_ok && g_service_ready.load();

    j["status"] = overall ? "ready" : "not_ready";
    j["checks"]["libpostal"] = parser_ok ? "ok" : "not_initialized";
    j["checks"]["cache"]     = g_config.cache_enabled ? "enabled" : "disabled";

    // Report LLM status including kill-switch
    if (g_llm.isDisabledByFlag()) {
        j["checks"]["llm"] = "disabled_by_DISABLE_LLM_flag";
    } else if (g_config.llm_enabled) {
        j["checks"]["llm"] = g_llm.isReady() ? "ok" : "not_loaded";
    } else {
        j["checks"]["llm"] = "disabled_in_config";
    }

    auto resp = drogon::HttpResponse::newHttpJsonResponse(j);
    if (!overall) resp->setStatusCode(drogon::k503ServiceUnavailable);
    callback(resp);
}

void HealthController::startup(const drogon::HttpRequestPtr& req,
                                std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
    Json::Value j;
    bool ready         = g_parser.isReady();
    j["status"]        = ready ? "started" : "starting";
    j["libpostal_loaded"] = ready;
    auto resp = drogon::HttpResponse::newHttpJsonResponse(j);
    if (!ready) resp->setStatusCode(drogon::k503ServiceUnavailable);
    callback(resp);
}

void HealthController::info(const drogon::HttpRequestPtr& req,
                             std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
    Json::Value j;
    auto& metrics = MetricsCollector::instance();

    j["version"] = "1.0.0";
    j["service"] = "address-normalization-service";

    // Config
    j["config"]["port"]                 = g_config.port;
    j["config"]["threads"]              = g_config.threads;
    j["config"]["cache_enabled"]        = g_config.cache_enabled;
    j["config"]["cache_max_entries"]    = static_cast<Json::UInt64>(g_config.cache_max_entries);
    j["config"]["llm_enabled"]          = g_config.llm_enabled;
    j["config"]["batch_max_size"]       = g_config.batch_max_size;
    j["config"]["confidence_threshold"] = g_config.llm_confidence_threshold;

    // -------------------------------------------------------------------------
    //  LLM kill-switch status — visible at /health/info for ops visibility
    // -------------------------------------------------------------------------
    {
        const char* flag = std::getenv("DISABLE_LLM");
        bool kill_active = flag &&
                           (std::string(flag) == "true" ||
                            std::string(flag) == "1"    ||
                            std::string(flag) == "yes");

        j["llm"]["kill_switch_active"]   = kill_active;
        j["llm"]["kill_switch_env_var"]  = "DISABLE_LLM";
        j["llm"]["kill_switch_value"]    = flag ? flag : "(not set)";
        j["llm"]["phase4_active"]        = g_llm.isReady();
        j["llm"]["description"]          = kill_active
            ? "Phase 4 bypassed — pipeline runs Phases 1-2-3 only (C++ core)"
            : (g_llm.isReady()
               ? "Phase 4 active — LLM inference enabled"
               : "Phase 4 inactive — LLM not loaded or disabled in config");
    }

    // Runtime stats
    j["stats"]["total_requests"]            = static_cast<Json::UInt64>(metrics.total_requests.load());
    j["stats"]["total_addresses_processed"] = static_cast<Json::UInt64>(metrics.total_addresses_processed.load());
    j["stats"]["parse_success"]             = static_cast<Json::UInt64>(metrics.total_parse_success.load());
    j["stats"]["parse_errors"]              = static_cast<Json::UInt64>(metrics.total_parse_errors.load());
    j["stats"]["batch_requests"]            = static_cast<Json::UInt64>(metrics.total_batch_requests.load());
    j["stats"]["llm_fallbacks"]             = static_cast<Json::UInt64>(metrics.total_llm_fallbacks.load());

    // Cache stats
    j["cache"]["size"]      = static_cast<Json::UInt64>(g_cache->size());
    j["cache"]["hits"]      = static_cast<Json::UInt64>(g_cache->hits());
    j["cache"]["misses"]    = static_cast<Json::UInt64>(g_cache->misses());
    j["cache"]["hit_ratio"] = g_cache->hitRatio();

    // Pipeline phase distribution
    j["pipeline"]["phase1"] = static_cast<Json::UInt64>(metrics.phase1_count.load());
    j["pipeline"]["phase2"] = static_cast<Json::UInt64>(metrics.phase2_count.load());
    j["pipeline"]["phase3"] = static_cast<Json::UInt64>(metrics.phase3_count.load());
    j["pipeline"]["phase4"] = static_cast<Json::UInt64>(metrics.phase4_count.load());

    // Environment
    const char* pod  = std::getenv("POD_NAME");
    const char* node = std::getenv("NODE_NAME");
    if (pod)  j["environment"]["pod"]  = pod;
    if (node) j["environment"]["node"] = node;

    LOG_F(1, "[health/info] requested"); // verbosity 1 = debug, won't spam logs but visible when needed for troubleshooting

    callback(drogon::HttpResponse::newHttpJsonResponse(j));
}

} // namespace addr
