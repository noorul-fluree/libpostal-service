#pragma once

// =============================================================================
//  Logger.h — thin wrapper around loguru
//
//  Usage:
//    #include "utils/Logger.h"
//
//    LOG_INFO("Service started on port {}", port);
//    LOG_WARN("Cache miss rate high: {:.1f}%", rate);
//    LOG_ERR("Parse failed for address: {}", addr);
//
//  Log file: ./logs/service_YYYYMMDD_HHMMSS.log
//  Rotation: max 20 files — oldest 5 deleted when limit reached
//  Both loguru file + std::cout kept (loguru also prints to stderr by default)
// =============================================================================

#include "loguru.hpp"
#include <string>
#include <filesystem>
#include <vector>
#include <algorithm>
#include <ctime>
#include <sstream>
#include <iomanip>

namespace addr {

// Convenience macros — prefixed ADDR_ to avoid conflict with Drogon/trantor
// trantor defines: LOG_DEBUG, LOG_INFO, LOG_WARN, LOG_ERROR
// We define only ADDR_ prefixed versions to be safe
#define ADDR_LOG_INFO(...)  LOG_F(INFO,    __VA_ARGS__)
#define ADDR_LOG_WARN(...)  LOG_F(WARNING, __VA_ARGS__)
#define ADDR_LOG_ERR(...)   LOG_F(ERROR,   __VA_ARGS__)
#define ADDR_LOG_DEBUG(...) LOG_F(1,       __VA_ARGS__)

class ServiceLogger {
public:
    // ==========================================================================
    //  initialize — call once from main() before anything else
    //  Creates a timestamped log file, enforces 20-file max (deletes oldest 5)
    // ==========================================================================
    static void initialize(int argc, char* argv[],
                           const std::string& log_dir = "./logs") {
        namespace fs = std::filesystem;
        fs::create_directories(log_dir);

        // -----------------------------------------------------------------------
        //  Enforce max 20 log files — delete oldest 5 if exceeded
        // -----------------------------------------------------------------------
        pruneLogFiles(log_dir, 20, 5);

        // -----------------------------------------------------------------------
        //  Build timestamped filename: service_20260401_143022.log
        // -----------------------------------------------------------------------
        auto now = std::time(nullptr);
        std::tm tm_buf{};
        localtime_r(&now, &tm_buf);
        std::ostringstream oss;
        oss << log_dir << "/service_"
            << std::put_time(&tm_buf, "%Y%m%d_%H%M%S")
            << ".log";
        std::string log_path = oss.str();

        // -----------------------------------------------------------------------
        //  Init loguru
        //  - stderr verbosity: INFO and above
        //  - file verbosity:   everything (DEBUG included)
        // -----------------------------------------------------------------------
        loguru::init(argc, argv);
        loguru::g_stderr_verbosity = loguru::Verbosity_INFO;
        loguru::add_file(log_path.c_str(),
                         loguru::Append,
                         loguru::Verbosity_MAX);

        LOG_F(INFO, "=== Address Normalization Service starting ===");
        LOG_F(INFO, "Log file: %s", log_path.c_str());
    }

private:
    // -----------------------------------------------------------------------
    //  pruneLogFiles
    //  If directory has >= max_files log files, delete the oldest del_count.
    // -----------------------------------------------------------------------
    static void pruneLogFiles(const std::string& log_dir,
                               size_t max_files,
                               size_t del_count) {
        namespace fs = std::filesystem;
        std::vector<fs::directory_entry> logs;

        for (const auto& entry : fs::directory_iterator(log_dir)) {
            if (entry.is_regular_file()) {
                auto name = entry.path().filename().string();
                if (name.rfind("service_", 0) == 0 &&
                    name.size() > 4 &&
                    name.substr(name.size() - 4) == ".log") {
                    logs.push_back(entry);
                }
            }
        }

        if (logs.size() < max_files) return;

        // Sort oldest first (by filename which is timestamp-based)
        std::sort(logs.begin(), logs.end(),
            [](const fs::directory_entry& a, const fs::directory_entry& b) {
                return a.path().filename() < b.path().filename();
            });

        size_t to_delete = std::min(del_count, logs.size());
        for (size_t i = 0; i < to_delete; ++i) {
            std::error_code ec;
            fs::remove(logs[i].path(), ec);
            if (!ec)
                fprintf(stderr, "[Logger] Deleted old log: %s\n",
                        logs[i].path().filename().c_str());
        }
    }
};

} // namespace addr