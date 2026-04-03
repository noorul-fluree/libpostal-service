#pragma once

#include <drogon/HttpFilter.h>
#include <string>
#include <unordered_set>
#include <mutex>

namespace addr {

// =============================================================================
//  ApiKeyFilter — validates API key on every request
//
//  Supports 3 modes (configured via ServiceConfig):
//    1. DISABLED:  No auth required (dev/testing)
//    2. SINGLE:    One master key from config/env
//    3. MULTI:     Multiple keys loaded from file
//
//  API key can be passed via:
//    - Header:  X-API-Key: <key>
//    - Header:  Authorization: Bearer <key>
//    - Query:   ?api_key=<key>
//
//  Health and metrics endpoints are always exempt.
// =============================================================================
class ApiKeyFilter : public drogon::HttpFilter<ApiKeyFilter> {
public:
    void doFilter(const drogon::HttpRequestPtr& req,
                  drogon::FilterCallback&& failCb,
                  drogon::FilterChainCallback&& passCb) override;

    // Load API keys (called from main.cc during init)
    static void initialize(bool enabled, const std::string& master_key,
                           const std::string& keys_file = "");

    // Add a key at runtime (e.g., from admin endpoint)
    static void addKey(const std::string& key);

    // Check if a key is valid
    static bool isValidKey(const std::string& key);

    static bool isEnabled() { return enabled_; }

private:
    static bool enabled_;
    static std::string master_key_;
    static std::unordered_set<std::string> valid_keys_;
    static std::mutex keys_mutex_;

    // Extract API key from request
    static std::string extractKey(const drogon::HttpRequestPtr& req);

    // Check if path is exempt from auth
    static bool isExemptPath(const std::string& path);
};

} // namespace addr
