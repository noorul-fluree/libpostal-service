#pragma once

#include <drogon/HttpFilter.h>
#include "controllers/ApiKeyFilter.h"
#include "controllers/JwtAuthFilter.h"

namespace addr {

// =============================================================================
//  AuthFilter — unified auth that delegates based on configured mode
//
//  Modes:
//    "disabled"  → no auth (dev/testing)
//    "api_key"   → ApiKeyFilter only (Phase 1)
//    "jwt"       → JwtAuthFilter only (Phase 2 with Node.js auth service)
//    "both"      → try JWT first, fall back to API key (migration period)
//
//  Register this filter on routes instead of ApiKeyFilter or JwtAuthFilter
//  directly. The mode is set once at startup from config.
// =============================================================================
class AuthFilter : public drogon::HttpFilter<AuthFilter> {
public:
    void doFilter(const drogon::HttpRequestPtr& req,
                  drogon::FilterCallback&& failCb,
                  drogon::FilterChainCallback&& passCb) override;

    // Set mode at startup
    static void setMode(const std::string& mode) { mode_ = mode; }
    static std::string getMode() { return mode_; }

private:
    static std::string mode_;  // "disabled", "api_key", "jwt", "both"

    static bool isExemptPath(const std::string& path);
    static std::string extractBearerToken(const drogon::HttpRequestPtr& req);
    static std::string extractApiKey(const drogon::HttpRequestPtr& req);
};

} // namespace addr
