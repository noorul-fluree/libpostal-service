#include "services/GeoNamesDB.h"
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

// =============================================================================
//  Global service instances
// =============================================================================
addr::AddressParser     g_parser;
addr::PreProcessor      g_preprocessor;
addr::ConfidenceScorer  g_scorer;
addr::RuleEngine        g_rules;
addr::CacheManager*     g_cache = nullptr;  // FIX: pointer — mutex/atomic not move-assignable
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
              << "  ├─ POST  http://0.0.0.0:" << g_config.port << "/api/v1/enrich/geo (max " << g_config.batch_max_size << " records)\n"
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
    if (const char* v = std::getenv("GEONAMES_DATA_DIR")) g_config.geonames_data_dir = v;
    if (const char* v = std::getenv("CACHE_MAX_SIZE"))     g_config.cache_max_entries = std::stoull(v);
    if (const char* v = std::getenv("AUTH_ENABLED"))       g_config.auth_enabled = (std::string(v) == "true");
    if (const char* v = std::getenv("AUTH_MODE"))          g_config.auth_mode = v;

    // LLM always disabled in this build - DELETE/comment if you want LLM Enable
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

    // Language classifier — only if normalize endpoint needed
    const char* env_classifier = std::getenv("ENABLE_LANGUAGE_CLASSIFIER");

    // Instead of setting the env var every time, just change the default in main.cc
    // Find this line:
    // if (env_classifier && std::string(env_classifier) == "true") {

    // Change to: always load unless explicitly disabled
    if (!env_classifier || std::string(env_classifier) != "false") {       
        if (!g_parser.initLanguageClassifier())
            std::cerr << "[main] WARNING: language classifier failed\n";
    } else {
        std::cout << "[main] language classifier skipped (set ENABLE_LANGUAGE_CLASSIFIER=true to enable)\n";
    }

    // 2. Cache — FIX: new to avoid move-assignment of mutex/atomic
    if (g_config.cache_enabled) {
        g_cache = new addr::CacheManager(g_config.cache_max_entries, g_config.cache_ttl_seconds);
        std::cout << "[main] Cache initialized (max " << g_config.cache_max_entries << " entries)\n";
    }

    // 3. Stateless services
    std::cout << "[main] Pre-processor ready\n"
              << "[main] Rule engine ready\n"
              << "[main] Confidence scorer ready\n";

    // 4. LLM — disabled, just log it
    std::cout << "[main] LLM fallback disabled — Phase 4 inactive\n";


    // extended for GeoNames geo lookup DB
    if (!g_config.geonames_data_dir.empty()) {
        std::cout << "[main] Initializing GeoNamesDB...\n";
         if (addr::GeoNamesDB::instance().initialize(g_config.geonames_data_dir)) {
            std::cout << "[main] GeoNamesDB ready ("
                      << addr::GeoNamesDB::instance().postalEntries() << " postal, "
                      << addr::GeoNamesDB::instance().cityEntries()   << " city entries)\n";
        }    
        else {
            std::cerr << "[main] WARNING: GeoNamesDB failed to load — "
                      << "/api/v1/enrich/geo will run without geo lookup\n";
        }
    } else {
        std::cout << "[main] GeoNamesDB skipped — geonames_data_dir not configured\n"
                  << "[main] Set geonames_data_dir in config.json to enable /api/v1/enrich/geo\n";
    }

    // 5. Authentication
    {
        const char* env_api_key   = std::getenv("API_KEY");
        const char* env_keys_file = std::getenv("API_KEYS_FILE");
        const char* env_jwt_url   = std::getenv("JWT_AUTH_URL");

        if (!g_config.auth_enabled) {
            addr::AuthFilter::setMode("disabled");
            std::cout << "[main] Authentication: DISABLED\n";
        } else {
            addr::AuthFilter::setMode(g_config.auth_mode);
            std::cout << "[main] Authentication: ENABLED (mode=" << g_config.auth_mode << ")\n";

            if (g_config.auth_mode == "api_key" || g_config.auth_mode == "both") {
                std::string api_key   = env_api_key   ? env_api_key   : g_config.auth_api_key;
                std::string keys_file = env_keys_file ? env_keys_file : g_config.auth_keys_file;
                addr::ApiKeyFilter::initialize(true, api_key, keys_file);
            }
            if (g_config.auth_mode == "jwt" || g_config.auth_mode == "both") {
                std::string jwt_url = env_jwt_url ? env_jwt_url : g_config.jwt_auth_url;
                addr::JwtAuthFilter::initialize(
                    jwt_url,
                    g_config.jwt_auth_timeout_ms,
                    g_config.jwt_cache_ttl_seconds,
                    g_config.jwt_cache_max_entries
                );
            }
        }
    }

    g_service_ready.store(true);
    std::cout << "[main] All services initialized, starting HTTP server...\n";

    // =========================================================================
    //  Drogon
    // =========================================================================
    system("mkdir -p ./logs ./uploads");

    auto& app = drogon::app();
    app.setLogPath("./logs")
       .setLogLevel(trantor::Logger::kInfo)
       .addListener("0.0.0.0", g_config.port)
       .setThreadNum(num_threads)
       .setMaxConnectionNum(g_config.max_connections)
       .setMaxConnectionNumPerIP(0)
       .setIdleConnectionTimeout(60)
       .setKeepaliveRequestsNumber(10000)
       .setGzipStatic(true)
       .enableGzip(true)
       .setClientMaxBodySize(10 * 1024 * 1024)
       .setUploadPath("./uploads");

    // CORS preflight
    app.registerPreRoutingAdvice([](const drogon::HttpRequestPtr& req,
                                    drogon::FilterCallback&& stop,
                                    drogon::FilterChainCallback&& pass) {
        if (req->method() == drogon::Options) {
            auto resp = drogon::HttpResponse::newHttpResponse();
            resp->addHeader("Access-Control-Allow-Origin", "*");
            resp->addHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
            resp->addHeader("Access-Control-Allow-Headers", "Content-Type, Authorization, X-API-Key");
            resp->addHeader("Access-Control-Max-Age", "86400");
            stop(resp);
        } else { pass(); }
    });

    // CORS headers on all responses
    app.registerPostHandlingAdvice([](const drogon::HttpRequestPtr&,
                                      const drogon::HttpResponsePtr& resp) {
        resp->addHeader("Access-Control-Allow-Origin", "*");
        resp->addHeader("X-Service", "address-normalization-service");
    });

    // FIX: registerPreServerShutdownAdvice does not exist in Drogon 1.9.11
    // Shutdown handled in signalHandler above instead.

    std::cout << "\n[main] Server listening on 0.0.0.0:" << g_config.port
              << " with " << num_threads << " worker threads\n\n";

    app.run();

    delete g_cache;
    g_cache = nullptr;
    return 0;
}