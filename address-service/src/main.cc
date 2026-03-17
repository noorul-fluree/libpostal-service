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
#include "services/MetricsCollector.h"

// =============================================================================
//  Global service instances (shared across all controller threads)
// =============================================================================
addr::AddressParser     g_parser;
addr::PreProcessor      g_preprocessor;
addr::ConfidenceScorer  g_scorer;
addr::RuleEngine        g_rules;
addr::CacheManager*     g_cache = nullptr;   // FIX: pointer — mutex/atomic not move-assignable
addr::ServiceConfig     g_config;
std::atomic<bool>       g_service_ready{false};

// =============================================================================
//  Signal handler for graceful shutdown
// =============================================================================
static void signalHandler(int sig) {
    std::cout << "\n[main] Received signal " << sig << ", shutting down..." << std::endl;
    g_service_ready.store(false);
    g_parser.shutdown();
    drogon::app().quit();
}

// =============================================================================
//  Load configuration from JSON file
// =============================================================================
static addr::ServiceConfig loadConfig(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        std::cout << "[main] Config file not found at " << path
                  << ", using defaults" << std::endl;
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
    const char* env_threads = std::getenv("DROGON_THREADS");
    if (env_threads) {
        int t = std::atoi(env_threads);
        if (t > 0) return std::max(1, t - 1);
    }
    int hw = static_cast<int>(std::thread::hardware_concurrency());
    return std::max(1, hw - 1);
}

// =============================================================================
//  Print startup banner
// =============================================================================
static void printBanner(const addr::ServiceConfig& cfg, int threads) {
    std::cout << "\n"
              << "====================================================\n"
              << "  Address Normalization Service v1.0.0\n"
              << "====================================================\n"
              << "  Port:           " << cfg.port << "\n"
              << "  Worker threads: " << threads << "\n"
              << "  Cache:          " << (cfg.cache_enabled ? "enabled" : "disabled");
    if (cfg.cache_enabled) {
        std::cout << " (max " << cfg.cache_max_entries << " entries, TTL "
                  << cfg.cache_ttl_seconds << "s)";
    }
    std::cout << "\n"
              << "  Rule engine:    " << (cfg.rules_enabled ? "enabled" : "disabled") << "\n"
              << "  LLM fallback:   disabled\n"
              << "  Batch max:      " << cfg.batch_max_size << " addresses\n"
              << "  libpostal data: " << cfg.libpostal_data_dir << "\n"
              << "====================================================\n\n";
}

// =============================================================================
//  Main
// =============================================================================
int main(int argc, char* argv[]) {
    std::string config_path = "config/config.json";  // local default for dev
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if ((arg == "--config" || arg == "-c") && i + 1 < argc) {
            config_path = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: address-service [options]\n"
                      << "  -c, --config <path>  Path to config.json\n"
                      << "  -h, --help           Show this help\n";
            return 0;
        }
    }

    std::signal(SIGINT,  signalHandler);
    std::signal(SIGTERM, signalHandler);

    // Load config
    g_config = loadConfig(config_path);

    // Environment overrides
    if (const char* v = std::getenv("PORT"))              g_config.port = std::atoi(v);
    if (const char* v = std::getenv("LIBPOSTAL_DATA_DIR")) g_config.libpostal_data_dir = v;
    if (const char* v = std::getenv("CACHE_MAX_SIZE"))    g_config.cache_max_entries = std::stoull(v);

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

    // 2. Cache — FIX: construct via new to avoid move-assignment of mutex/atomic
    if (g_config.cache_enabled) {
        g_cache = new addr::CacheManager(g_config.cache_max_entries, g_config.cache_ttl_seconds);
        std::cout << "[main] Cache initialized (max " << g_config.cache_max_entries
                  << " entries)" << std::endl;
    }

    // 3. Stateless services
    std::cout << "[main] Pre-processor ready" << std::endl;
    std::cout << "[main] Rule engine ready" << std::endl;
    std::cout << "[main] Confidence scorer ready" << std::endl;

    g_service_ready.store(true);
    std::cout << "[main] All services initialized, starting HTTP server..." << std::endl;

    // =========================================================================
    //  Configure and start Drogon
    // =========================================================================
    auto& app = drogon::app();

    // Create logs dir if missing
    system("mkdir -p ./logs ./uploads");

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

    // CORS — preflight
    app.registerPreRoutingAdvice([](const drogon::HttpRequestPtr& req,
                                    drogon::FilterCallback&& stop,
                                    drogon::FilterChainCallback&& pass) {
        if (req->method() == drogon::Options) {
            auto resp = drogon::HttpResponse::newHttpResponse();
            resp->addHeader("Access-Control-Allow-Origin", "*");
            resp->addHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
            resp->addHeader("Access-Control-Allow-Headers", "Content-Type, Authorization");
            resp->addHeader("Access-Control-Max-Age", "86400");
            stop(resp);
        } else {
            pass();
        }
    });

    // CORS — all responses
    app.registerPostHandlingAdvice([](const drogon::HttpRequestPtr&,
                                      const drogon::HttpResponsePtr& resp) {
        resp->addHeader("Access-Control-Allow-Origin", "*");
        resp->addHeader("X-Service", "address-normalization-service");
    });

    // FIX: registerPreServerShutdownAdvice does not exist in Drogon 1.9.11
    // Shutdown is handled in signalHandler above instead.

    std::cout << "\n[main] Server listening on 0.0.0.0:" << g_config.port
              << " with " << num_threads << " worker threads\n" << std::endl;

    app.run();

    // Cleanup after app.run() returns
    delete g_cache;
    g_cache = nullptr;

    return 0;
}