#include "controllers/ApiKeyFilter.h"
#include <fstream>
#include <iostream>
#include <algorithm>

namespace addr {

bool ApiKeyFilter::enabled_ = false;
std::string ApiKeyFilter::master_key_;
std::unordered_set<std::string> ApiKeyFilter::valid_keys_;
std::mutex ApiKeyFilter::keys_mutex_;

void ApiKeyFilter::initialize(bool enabled, const std::string& master_key,
                               const std::string& keys_file) {
    enabled_ = enabled;

    if (!enabled) {
        std::cout << "[ApiKeyFilter] Authentication DISABLED (development mode)" << std::endl;
        return;
    }

    if (!master_key.empty()) {
        master_key_ = master_key;
        std::cout << "[ApiKeyFilter] Master API key configured" << std::endl;
    }

    // Load keys from file (one key per line)
    if (!keys_file.empty()) {
        std::ifstream file(keys_file);
        if (file.is_open()) {
            std::string line;
            int count = 0;
            while (std::getline(file, line)) {
                // Trim whitespace
                line.erase(0, line.find_first_not_of(" \t\r\n"));
                line.erase(line.find_last_not_of(" \t\r\n") + 1);
                // Skip empty lines and comments
                if (!line.empty() && line[0] != '#') {
                    std::lock_guard<std::mutex> lock(keys_mutex_);
                    valid_keys_.insert(line);
                    ++count;
                }
            }
            std::cout << "[ApiKeyFilter] Loaded " << count << " API keys from " << keys_file << std::endl;
        } else {
            std::cerr << "[ApiKeyFilter] WARNING: Could not open keys file: " << keys_file << std::endl;
        }
    }

    if (master_key_.empty() && valid_keys_.empty()) {
        std::cerr << "[ApiKeyFilter] WARNING: Auth enabled but no keys configured! "
                  << "All requests will be rejected." << std::endl;
    }

    std::cout << "[ApiKeyFilter] Authentication ENABLED" << std::endl;
}

void ApiKeyFilter::addKey(const std::string& key) {
    std::lock_guard<std::mutex> lock(keys_mutex_);
    valid_keys_.insert(key);
}

bool ApiKeyFilter::isValidKey(const std::string& key) {
    if (key.empty()) return false;

    // Check master key
    if (!master_key_.empty() && key == master_key_) return true;

    // Check key set
    std::lock_guard<std::mutex> lock(keys_mutex_);
    return valid_keys_.count(key) > 0;
}

std::string ApiKeyFilter::extractKey(const drogon::HttpRequestPtr& req) {
    // 1. Check X-API-Key header
    std::string key = req->getHeader("X-API-Key");
    if (!key.empty()) return key;

    // 2. Check Authorization: Bearer <key>
    std::string auth = req->getHeader("Authorization");
    if (auth.size() > 7 && auth.substr(0, 7) == "Bearer ") {
        return auth.substr(7);
    }

    // 3. Check query parameter
    key = req->getParameter("api_key");
    if (!key.empty()) return key;

    return "";
}

bool ApiKeyFilter::isExemptPath(const std::string& path) {
    // Health and metrics endpoints are always accessible
    if (path.find("/health/") == 0) return true;
    if (path == "/health/live") return true;
    if (path == "/health/ready") return true;
    if (path == "/health/startup") return true;
    if (path == "/metrics") return true;
    return false;
}

void ApiKeyFilter::doFilter(const drogon::HttpRequestPtr& req,
                             drogon::FilterCallback&& failCb,
                             drogon::FilterChainCallback&& passCb) {
    // If auth is disabled, pass everything
    if (!enabled_) {
        passCb();
        return;
    }

    // Exempt paths (health checks, metrics)
    if (isExemptPath(req->path())) {
        passCb();
        return;
    }

    // Extract and validate key
    std::string key = extractKey(req);
    if (isValidKey(key)) {
        passCb();
        return;
    }

    // Reject
    Json::Value err;
    err["error"] = "Unauthorized";
    err["message"] = "Valid API key required. Pass via X-API-Key header, "
                     "Authorization: Bearer <key>, or ?api_key=<key> query parameter";

    auto resp = drogon::HttpResponse::newHttpJsonResponse(err);
    resp->setStatusCode(drogon::k401Unauthorized);
    resp->addHeader("WWW-Authenticate", "Bearer");
    failCb(resp);
}

} // namespace addr
