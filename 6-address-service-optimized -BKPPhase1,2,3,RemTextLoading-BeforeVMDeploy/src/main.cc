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
    std::cout << "\n[main] Received signal " << sig << ", shutting down..." << std::endl;
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
        std::cout << "[main] Config file not found at " << path << ", using defaults" << std::endl;
        return addr::ServiceConfig();
    }
    Json::Value root;
    Json::CharReaderBuilder builder;
    std::string errors;
    if (!Json::parseFromStream(builder, file, &root, &errors)) {
        std::cerr << "[main] ERROR parsing config: " << errors << std::endl;
        return addr::ServiceConfig();
    }
    std::cout << "[main] Configuration loaded from " << path << std::endl;
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
    std::cout << "\n"
              << "====================================================\n"
              << "  Address Normalization Service v1.0.0\n"
              << "====================================================\n"
              << "  Port:           " << cfg.port << "\n"
              << "  Worker threads: " << threads << "\n"
              << "  Cache:          " << (cfg.cache_enabled ? "enabled" : "disabled");
    if (cfg.cache_enabled)
        std::cout << " (max " << cfg.cache_max_entries
                  << " entries, TTL " << cfg.cache_ttl_seconds << "s)";
    std::cout << "\n"
              << "  Rule engine:    " << (cfg.rules_enabled ? "enabled" : "disabled") << "\n"
              << "  LLM fallback:   disabled\n"
              << "  Auth:           " << (cfg.auth_enabled ? "ENABLED" : "disabled") << "\n"
              << "  Max addr len:   " << cfg.max_address_length << " chars\n"
              << "  Batch max:      " << cfg.batch_max_size << " addresses\n"
              << "  libpostal data: " << cfg.libpostal_data_dir << "\n"
              << "====================================================\n"
              << "\n"
              << "  Endpoints available:\n"
              << "  ┌─ POST  http://0.0.0.0:" << cfg.port << "/api/v1/parse\n"
              << "  ├─ POST  http://0.0.0.0:" << cfg.port << "/api/v1/normalize\n"
              << "  ├─ POST  http://0.0.0.0:" << cfg.port << "/api/v1/batch   (max " << cfg.batch_max_size << " records)\n"
              << "  ├─ POST  http://0.0.0.0:" << cfg.port << "/api/v1/enrich  (max " << cfg.batch_max_size << " records)\n"
              << "  ├─ POST  http://0.0.0.0:" << cfg.port << "/api/v1/enrich/geo/lmdb (max " << cfg.batch_max_size << " records)\n"
              << "  ├─ GET   http://0.0.0.0:" << cfg.port << "/ref/v1/postal/{code}\n"
              << "  ├─ POST  http://0.0.0.0:" << cfg.port << "/ref/v1/postal/batch\n"
              << "  ├─ GET   http://0.0.0.0:" << cfg.port << "/ref/v1/country/{code}\n"
              << "  ├─ GET   http://0.0.0.0:" << cfg.port << "/ref/v1/state/{country}/{abbrev}\n"
              << "  ├─ GET   http://0.0.0.0:" << cfg.port << "/ref/v1/city/search\n"
              << "  ├─ POST  http://0.0.0.0:" << cfg.port << "/ref/v1/enrich\n"
              << "  ├─ POST  http://0.0.0.0:" << cfg.port << "/ref/v1/validate\n"
              << "  ├─ GET   http://0.0.0.0:" << cfg.port << "/ref/v1/postal/reverse\n"
              << "  ├─ POST  http://0.0.0.0:" << cfg.port << "/ref/v1/abbreviation/expand\n"
              << "  ├─ GET   http://0.0.0.0:" << cfg.port << "/health/live\n"
              << "  ├─ GET   http://0.0.0.0:" << cfg.port << "/health/ready\n"
              << "  ├─ GET   http://0.0.0.0:" << cfg.port << "/health/startup\n"
              << "  ├─ GET   http://0.0.0.0:" << cfg.port << "/health/info\n"
              << "  └─ GET   http://0.0.0.0:" << cfg.port << "/metrics\n"
              << "\n"
              << "====================================================\n\n";
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
            std::cout << "Usage: address-service [options]\n"
                      << "  -c, --config <path>  Path to config.json\n"
                      << "  -h, --help           Show this help\n";
            return 0;
        }
    }

    std::signal(SIGINT,  signalHandler);
    std::signal(SIGTERM, signalHandler);

    g_config = loadConfig(config_path);

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
    std::cout << "[main] Initializing libpostal (this takes 15-30s)..." << std::endl;
    if (!g_parser.initialize(g_config.libpostal_data_dir)) {
        std::cerr << "[main] FATAL: Failed to initialize libpostal. Exiting." << std::endl;
        return 1;
    }
    std::cout << "[main] libpostal ready" << std::endl;

    // Language classifier
    const char* env_classifier = std::getenv("ENABLE_LANGUAGE_CLASSIFIER");
    if (!env_classifier || std::string(env_classifier) != "false") {
        if (!g_parser.initLanguageClassifier())
            std::cerr << "[main] WARNING: language classifier failed\n";
    } else {
        std::cout << "[main] language classifier skipped\n";
    }

    // 2. Cache
    if (g_config.cache_enabled) {
        g_cache = new addr::CacheManager(g_config.cache_max_entries, g_config.cache_ttl_seconds);
        std::cout << "[main] Cache initialized (max " << g_config.cache_max_entries << " entries)\n";
    }

    // 3. Stateless services
    std::cout << "[main] Pre-processor ready\n"
              << "[main] Rule engine ready\n"
              << "[main] Confidence scorer ready\n";

    // 4. LLM — disabled
    std::cout << "[main] LLM fallback disabled — Phase 4 inactive\n";

    // 5. GeoNames LMDB
    if (!g_config.geonames_lmdb_dir.empty()) {
        std::cout << "[main] Initializing GeoNamesLMDB...\n";
        if (addr::GeoNamesLMDB::instance().initialize(g_config.geonames_lmdb_dir)) {
            std::cout << "[main] GeoNamesLMDB ready"
                      << " | dir=" << g_config.geonames_lmdb_dir << "\n";
        } else {
            std::cerr << "[main] WARNING: GeoNamesLMDB failed to open — "
                      << "/api/v1/enrich/geo/lmdb will run without geo lookup\n";
        }
    } else {
        std::cout << "[main] GeoNamesLMDB skipped — geonames_lmdb.lmdb_dir not set\n";
    }

    // 6. Authentication
    if (g_config.auth_enabled) {
        std::cout << "[main] Authentication: ENABLED (mode=" << g_config.auth_mode << ")\n";
    } else {
        std::cout << "[main] Authentication: DISABLED\n";
    }

    // =========================================================================
    //  Start Drogon
    // =========================================================================
    g_service_ready.store(true);
    LOG_F(INFO, "All services initialized, starting HTTP server...");
    std::cout << "[main] All services initialized, starting HTTP server...\n";

    system("mkdir -p ./logs ./uploads");

    // drogon::app()
    //     .setLogPath("./logs")
    //     .setLogLevel(trantor::Logger::kWarn)
    //     .addListener("0.0.0.0", g_config.port)
    //     .setThreadNum(num_threads)
    //     .setMaxConnectionNum(100000)
    //     .setMaxConnectionNumPerIP(0)
    //     .setIdleConnectionTimeout(60)
    //     .run(); was getting - 413   Request Entity Too Large

    drogon::app()
    .setLogPath("./logs")
    .setLogLevel(trantor::Logger::kWarn)
    .addListener("0.0.0.0", g_config.port)
    .setThreadNum(num_threads)
    .setMaxConnectionNum(100000)
    .setMaxConnectionNumPerIP(0)
    .setIdleConnectionTimeout(60)
    .setClientMaxBodySize(256 * 1024 * 1024)  // 256MB
    .run();

    std::cout << "[main] Server listening on 0.0.0.0:"
              << g_config.port << " with " << num_threads
              << " worker threads\n";

    return 0;
}