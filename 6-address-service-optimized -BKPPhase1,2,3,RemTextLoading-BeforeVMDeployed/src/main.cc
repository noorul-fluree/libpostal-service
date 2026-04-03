#include "services/GeoNamesLMDB.h"
#include <drogon/drogon.h>
#include <iostream>
#include <fstream>
#include <csignal>
#include <thread>
#include <json/json.h>

#include "models/AddressModels.h"
#include "services/AddressParser.h"
#include "services/PreProcessor.h"
#include "services/ConfidenceScorer.h"
#include "services/RuleEngine.h"
#include "services/CacheManager.h"
#include "services/LLMFallback.h"
#include "services/MetricsCollector.h"
#include "controllers/ApiKeyFilter.h"
#include "controllers/JwtAuthFilter.h"
#include "controllers/AuthFilter.h"
#include "utils/Logger.h"

// =============================================================================
//  Global service instances
// =============================================================================
addr::AddressParser     g_parser;
addr::PreProcessor      g_preprocessor;
addr::ConfidenceScorer  g_scorer;
addr::RuleEngine        g_rules;
addr::CacheManager*     g_cache = nullptr;
addr::LLMFallback       g_llm;
addr::ServiceConfig     g_config;
std::atomic<bool>       g_service_ready{false};

// =============================================================================
//  Signal handler
// =============================================================================
static void signalHandler(int sig) {
    LOG_F(INFO, "[main] Received signal %d, shutting down...", sig);
    g_service_ready.store(false);
    g_llm.shutdown();
    g_parser.shutdown();
    drogon::app().quit();
}

// =============================================================================
//  Load configuration
// =============================================================================
static addr::ServiceConfig loadConfig(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        LOG_F(WARNING, "[main] Config file not found at %s, using defaults", path.c_str());
        return addr::ServiceConfig();
    }
    Json::Value root;
    Json::CharReaderBuilder builder;
    std::string errors;
    if (!Json::parseFromStream(builder, file, &root, &errors)) {
        LOG_F(ERROR, "[main] ERROR parsing config: %s", errors.c_str());
        return addr::ServiceConfig();
    }
    LOG_F(INFO, "[main] Configuration loaded from %s", path.c_str());
    return addr::ServiceConfig::fromJson(root);
}

// =============================================================================
//  Detect thread count
// =============================================================================
static int detectThreads(int configured) {
    if (configured > 0) return configured;
    const char* env = std::getenv("DROGON_THREADS");
    if (env) { int t = std::atoi(env); if (t > 0) return std::max(1, t - 1); }
    return std::max(1, static_cast<int>(std::thread::hardware_concurrency()) - 1);
}

// =============================================================================
//  Print banner
// =============================================================================
static void printBanner(const addr::ServiceConfig& cfg, int threads) {
    LOG_F(INFO, "====================================================");
    LOG_F(INFO, "  Address Normalization Service v1.0.0");
    LOG_F(INFO, "====================================================");
    LOG_F(INFO, "  Port:           %d", cfg.port);
    LOG_F(INFO, "  Worker threads: %d", threads);
    LOG_F(INFO, "  Cache:          %s%s",
          cfg.cache_enabled ? "enabled" : "disabled",
          cfg.cache_enabled ? (" (max " + std::to_string(cfg.cache_max_entries) +
                               " entries, TTL " + std::to_string(cfg.cache_ttl_seconds) + "s)").c_str() : "");
    LOG_F(INFO, "  Rule engine:    %s", cfg.rules_enabled ? "enabled" : "disabled");
    LOG_F(INFO, "  LLM fallback:   disabled");
    LOG_F(INFO, "  Auth:           %s", cfg.auth_enabled ? "ENABLED" : "disabled");
    LOG_F(INFO, "  Max addr len:   %d chars", cfg.max_address_length);
    LOG_F(INFO, "  Batch max:      %d addresses", cfg.batch_max_size);
    LOG_F(INFO, "  libpostal data: %s", cfg.libpostal_data_dir.c_str());
    LOG_F(INFO, "====================================================");
    LOG_F(INFO, "  Endpoints available:");
    LOG_F(INFO, "  POST  /api/v1/parse");
    LOG_F(INFO, "  POST  /api/v1/normalize");
    LOG_F(INFO, "  POST  /api/v1/batch   (max %d records)", cfg.batch_max_size);
    LOG_F(INFO, "  POST  /api/v1/enrich  (max %d records)", cfg.batch_max_size);
    LOG_F(INFO, "  POST  /api/v1/enrich/geo/lmdb (max %d records)", cfg.batch_max_size);
    LOG_F(INFO, "  GET   /ref/v1/postal/{code}");
    LOG_F(INFO, "  POST  /ref/v1/postal/batch");
    LOG_F(INFO, "  GET   /ref/v1/country/{code}");
    LOG_F(INFO, "  GET   /ref/v1/state/{country}/{abbrev}");
    LOG_F(INFO, "  GET   /ref/v1/city/search");
    LOG_F(INFO, "  POST  /ref/v1/enrich");
    LOG_F(INFO, "  POST  /ref/v1/validate");
    LOG_F(INFO, "  GET   /ref/v1/postal/reverse");
    LOG_F(INFO, "  POST  /ref/v1/abbreviation/expand");
    LOG_F(INFO, "  GET   /health/live");
    LOG_F(INFO, "  GET   /health/ready");
    LOG_F(INFO, "  GET   /health/startup");
    LOG_F(INFO, "  GET   /health/info");
    LOG_F(INFO, "  GET   /metrics");
    LOG_F(INFO, "====================================================");
}

// =============================================================================
//  Main
// =============================================================================
int main(int argc, char* argv[]) {
    addr::ServiceLogger::initialize(argc, argv);
    std::string config_path = "config/config.json";
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if ((arg == "--config" || arg == "-c") && i + 1 < argc) config_path = argv[++i];
        else if (arg == "--help" || arg == "-h") {
            LOG_F(INFO, "Usage: address-service [options]");
            LOG_F(INFO, "  -c, --config <path>  Path to config.json");
            LOG_F(INFO, "  -h, --help           Show this help");
            return 0;
        }
    }

    std::signal(SIGINT,  signalHandler);
    std::signal(SIGTERM, signalHandler);

    g_config = loadConfig(config_path);

    LOG_F(INFO, "[main] port=%d cache=%d geonames_lmdb='%s' batch_max=%d",
          g_config.port, (int)g_config.cache_enabled,
          g_config.geonames_lmdb_dir.c_str(), g_config.batch_max_size);

    // Environment overrides
    if (const char* v = std::getenv("PORT"))               g_config.port = std::atoi(v);
    if (const char* v = std::getenv("LIBPOSTAL_DATA_DIR")) g_config.libpostal_data_dir = v;
    if (const char* v = std::getenv("CACHE_MAX_SIZE"))     g_config.cache_max_entries = std::stoull(v);
    if (const char* v = std::getenv("AUTH_ENABLED"))       g_config.auth_enabled = (std::string(v) == "true");
    if (const char* v = std::getenv("AUTH_MODE"))          g_config.auth_mode = v;

    // LLM always disabled in this build
    g_config.llm_enabled = false;

    int num_threads = detectThreads(g_config.threads);
    printBanner(g_config, num_threads);

    // =========================================================================
    //  Initialize services
    // =========================================================================

    // 1. libpostal
    LOG_F(INFO, "[main] Initializing libpostal (this takes 15-30s)...");
    if (!g_parser.initialize(g_config.libpostal_data_dir)) {
        LOG_F(ERROR, "[main] FATAL: Failed to initialize libpostal. Exiting.");
        return 1;
    }
    LOG_F(INFO, "[main] libpostal ready");

    // 2. Language classifier
    const char* env_classifier = std::getenv("ENABLE_LANGUAGE_CLASSIFIER");
    if (!env_classifier || std::string(env_classifier) != "false") {
        if (!g_parser.initLanguageClassifier())
            LOG_F(WARNING, "[main] language classifier failed");
        else
            LOG_F(INFO, "[main] language classifier ready");
    } else {
        LOG_F(INFO, "[main] language classifier skipped");
    }

    // 3. Cache
    if (g_config.cache_enabled) {
        g_cache = new addr::CacheManager(g_config.cache_max_entries, g_config.cache_ttl_seconds);
        LOG_F(INFO, "[main] Cache initialized (max %zu entries)", g_config.cache_max_entries);
    }

    // 4. Stateless services
    LOG_F(INFO, "[main] Pre-processor, Rule engine, Confidence scorer ready");

    // 5. LLM — disabled
    LOG_F(INFO, "[main] LLM fallback disabled — Phase 4 inactive");

    // 6. GeoNames LMDB
    if (!g_config.geonames_lmdb_dir.empty()) {
        LOG_F(INFO, "[main] Initializing GeoNamesLMDB...");
        if (addr::GeoNamesLMDB::instance().initialize(g_config.geonames_lmdb_dir)) {
            LOG_F(INFO, "[main] GeoNamesLMDB ready | dir=%s", g_config.geonames_lmdb_dir.c_str());
        } else {
            LOG_F(WARNING, "[main] GeoNamesLMDB failed to open — geo lookup disabled");
        }
    } else {
        LOG_F(WARNING, "[main] GeoNamesLMDB skipped — lmdb_dir not set");
    }

    // 7. Authentication
    if (g_config.auth_enabled) {
        LOG_F(INFO, "[main] Authentication: ENABLED (mode=%s)", g_config.auth_mode.c_str());
    } else {
        LOG_F(INFO, "[main] Authentication: DISABLED");
    }

    // =========================================================================
    //  Start Drogon
    // =========================================================================
    g_service_ready.store(true);
    LOG_F(INFO, "All services initialized, starting HTTP server...");

    system("mkdir -p ./logs ./uploads");

    drogon::app()
        // .setLogPath("./logs") to aavoid generetng an extra log file since its default by dragon and no link with our api service.
        .setLogLevel(trantor::Logger::kWarn)
        .addListener("0.0.0.0", g_config.port)
        .setThreadNum(num_threads)
        .setMaxConnectionNum(100000)
        .setMaxConnectionNumPerIP(0)
        .setIdleConnectionTimeout(60)
        .setClientMaxBodySize(256 * 1024 * 1024)  // 256MB
        .run();

    return 0;
}