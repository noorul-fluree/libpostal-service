#include "controllers/AuthFilter.h"
#include <iostream>

namespace addr {

std::string AuthFilter::mode_ = "disabled";

bool AuthFilter::isExemptPath(const std::string& path) {
    return path.find("/health/") == 0 ||
           path == "/health/live" ||
           path == "/health/ready" ||
           path == "/health/startup" ||
           path == "/metrics";
}

std::string AuthFilter::extractBearerToken(const drogon::HttpRequestPtr& req) {
    std::string auth = req->getHeader("Authorization");
    if (auth.size() > 7 && auth.substr(0, 7) == "Bearer ") {
        return auth.substr(7);
    }
    return "";
}

std::string AuthFilter::extractApiKey(const drogon::HttpRequestPtr& req) {
    std::string key = req->getHeader("X-API-Key");
    if (!key.empty()) return key;
    key = req->getParameter("api_key");
    return key;
}

void AuthFilter::doFilter(const drogon::HttpRequestPtr& req,
                           drogon::FilterCallback&& failCb,
                           drogon::FilterChainCallback&& passCb) {
    // Disabled mode — pass everything
    if (mode_ == "disabled") {
        passCb();
        return;
    }

    // Exempt paths
    if (isExemptPath(req->path())) {
        passCb();
        return;
    }

    // ========================================
    //  API Key mode
    // ========================================
    if (mode_ == "api_key") {
        if (ApiKeyFilter::isEnabled()) {
            // Delegate to ApiKeyFilter
            ApiKeyFilter filter;
            filter.doFilter(req, std::move(failCb), std::move(passCb));
        } else {
            passCb();
        }
        return;
    }

    // ========================================
    //  JWT mode — call Node.js auth service
    // ========================================
    if (mode_ == "jwt") {
        if (JwtAuthFilter::isInitialized()) {
            JwtAuthFilter filter;
            filter.doFilter(req, std::move(failCb), std::move(passCb));
        } else {
            // JWT not initialized — reject
            Json::Value err;
            err["error"] = "Auth service not configured";
            err["message"] = "JWT auth is enabled but auth service URL is not set";
            auto resp = drogon::HttpResponse::newHttpJsonResponse(err);
            resp->setStatusCode(drogon::k503ServiceUnavailable);
            failCb(resp);
        }
        return;
    }

    // ========================================
    //  Both mode — try JWT first, fall back to API key
    //  This is useful during migration from API keys to JWT
    // ========================================
    if (mode_ == "both") {
        // If request has Bearer token, use JWT
        std::string bearer = extractBearerToken(req);
        if (!bearer.empty() && JwtAuthFilter::isInitialized()) {
            JwtAuthFilter filter;
            filter.doFilter(req, std::move(failCb), std::move(passCb));
            return;
        }

        // If request has API key, use API key auth
        std::string api_key = extractApiKey(req);
        if (!api_key.empty() && ApiKeyFilter::isEnabled()) {
            ApiKeyFilter filter;
            filter.doFilter(req, std::move(failCb), std::move(passCb));
            return;
        }

        // No credentials provided
        Json::Value err;
        err["error"] = "Unauthorized";
        err["message"] = "Provide either Authorization: Bearer <jwt> "
                         "or X-API-Key: <key>";
        auto resp = drogon::HttpResponse::newHttpJsonResponse(err);
        resp->setStatusCode(drogon::k401Unauthorized);
        resp->addHeader("WWW-Authenticate", "Bearer");
        failCb(resp);
        return;
    }

    // Unknown mode — pass through with warning
    std::cerr << "[AuthFilter] Unknown auth mode: " << mode_
              << ", passing through" << std::endl;
    passCb();
}

} // namespace addr
