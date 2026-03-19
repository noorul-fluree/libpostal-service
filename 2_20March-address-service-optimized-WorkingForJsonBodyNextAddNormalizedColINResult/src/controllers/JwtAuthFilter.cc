#include "controllers/JwtAuthFilter.h"
#include <functional>
#include <netinet/tcp.h> 
#include <iostream>
#include <sstream>

namespace addr {

// =============================================================================
//  Static member initialization
// =============================================================================
std::atomic<bool>    JwtAuthFilter::initialized_{false};
std::string          JwtAuthFilter::auth_url_;
int                  JwtAuthFilter::timeout_ms_      = 3000;
int                  JwtAuthFilter::cache_ttl_seconds_ = 300;
int                  JwtAuthFilter::cache_max_entries_  = 10000;

// Sharded token cache — NUM_SHARDS independent mutexes eliminate contention
// on the validation cache under high concurrency.
std::array<JwtAuthFilter::TokenCacheShard, JwtAuthFilter::NUM_CACHE_SHARDS>
    JwtAuthFilter::cache_shards_;

std::atomic<uint64_t> JwtAuthFilter::cache_hits_{0};
std::atomic<uint64_t> JwtAuthFilter::cache_misses_{0};
std::atomic<uint64_t> JwtAuthFilter::auth_calls_{0};
std::atomic<uint64_t> JwtAuthFilter::auth_failures_{0};

// =============================================================================
//  Initialization
// =============================================================================
void JwtAuthFilter::initialize(const std::string& auth_url,
                                int timeout_ms,
                                int cache_ttl_seconds,
                                int cache_max_entries) {
    auth_url_          = auth_url;
    timeout_ms_        = timeout_ms;
    cache_ttl_seconds_ = cache_ttl_seconds;
    cache_max_entries_ = cache_max_entries;
    initialized_.store(true);

    std::cout << "[JwtAuthFilter] Initialized"
              << " | auth_url="   << auth_url_
              << " | timeout="    << timeout_ms_    << "ms"
              << " | cache_ttl="  << cache_ttl_seconds_ << "s"
              << " | cache_max="  << cache_max_entries_
              << " | shards="     << NUM_CACHE_SHARDS
              << std::endl;
}

// =============================================================================
//  Fast FNV-1a hash for token string → uint64_t cache key
//  No heap alloc, branch-free, uniform distribution
// =============================================================================
uint64_t JwtAuthFilter::hashToken(const std::string& token) noexcept {
    uint64_t h = 14695981039346656037ULL;
    for (unsigned char c : token) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    return h;
}

// =============================================================================
//  Path exemption check
// =============================================================================
bool JwtAuthFilter::isExemptPath(const std::string& path) {
    return path.find("/health/") == 0 || path == "/metrics";
}

// =============================================================================
//  Token extraction
// =============================================================================
std::string JwtAuthFilter::extractBearerToken(const drogon::HttpRequestPtr& req) {
    const std::string& auth = req->getHeader("Authorization");
    if (auth.size() > 7 && auth[0]=='B' && auth[6]==' ')
        return auth.substr(7);
    return {};
}

// =============================================================================
//  Sharded cache operations
// =============================================================================
std::optional<JwtValidationResult>
JwtAuthFilter::checkCache(uint64_t token_hash) {
    auto& shard = cache_shards_[token_hash & (NUM_CACHE_SHARDS - 1)];
    std::lock_guard<std::mutex> lock(shard.mutex);

    auto it = shard.map.find(token_hash);
    if (it == shard.map.end()) return std::nullopt;

    auto now = std::chrono::steady_clock::now();
    auto age = std::chrono::duration_cast<std::chrono::seconds>(
                   now - it->second.cached_at).count();
    int  eff = std::min(cache_ttl_seconds_, it->second.ttl_seconds);

    if (age > eff) {
        shard.map.erase(it);
        return std::nullopt;
    }
    return it->second;
}

void JwtAuthFilter::cacheResult(uint64_t token_hash,
                                 const JwtValidationResult& result) {
    auto& shard = cache_shards_[token_hash & (NUM_CACHE_SHARDS - 1)];
    std::lock_guard<std::mutex> lock(shard.mutex);

    // Simple eviction: if shard is too big, remove all expired entries
    size_t max_per_shard = static_cast<size_t>(cache_max_entries_) / NUM_CACHE_SHARDS + 1;
    if (shard.map.size() >= max_per_shard) {
        auto now = std::chrono::steady_clock::now();
        for (auto it = shard.map.begin(); it != shard.map.end(); ) {
            auto age = std::chrono::duration_cast<std::chrono::seconds>(
                           now - it->second.cached_at).count();
            int eff = std::min(cache_ttl_seconds_, it->second.ttl_seconds);
            if (age > eff) it = shard.map.erase(it);
            else           ++it;
        }
        // If still full, evict the first entry (oldest-ish)
        if (shard.map.size() >= max_per_shard)
            shard.map.erase(shard.map.begin());
    }

    JwtValidationResult cached  = result;
    cached.cached_at            = std::chrono::steady_clock::now();
    shard.map[token_hash]       = std::move(cached);
}

// =============================================================================
//  callAuthService — unchanged logic, same as original
// =============================================================================
JwtValidationResult JwtAuthFilter::callAuthService(const std::string& token) {
    JwtValidationResult result;
    auth_calls_.fetch_add(1, std::memory_order_relaxed);

    try {
        std::string url    = auth_url_;
        std::string scheme = "http";
        std::string host;
        uint16_t    port   = 80;
        std::string path   = "/auth/jwt";

        if (url.find("https://") == 0) { scheme = "https"; url = url.substr(8); port = 443; }
        else if (url.find("http://") == 0) { url = url.substr(7); }

        auto path_pos = url.find('/');
        if (path_pos != std::string::npos) { path = url.substr(path_pos); url = url.substr(0, path_pos); }

        auto colon_pos = url.find(':');
        if (colon_pos != std::string::npos) {
            host = url.substr(0, colon_pos);
            port = static_cast<uint16_t>(std::stoi(url.substr(colon_pos + 1)));
        } else {
            host = url;
        }

        auto client = drogon::HttpClient::newHttpClient(
            scheme + "://" + host + ":" + std::to_string(port));
        client->setSockOptCallback([](int fd) {
            int flag = 1;
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
        });

        auto req = drogon::HttpRequest::newHttpJsonRequest(Json::Value());
        req->setMethod(drogon::Post);
        req->setPath(path);
        req->setContentTypeCode(drogon::CT_APPLICATION_JSON);

        Json::Value body;
        body["token"] = token;
        Json::StreamWriterBuilder writer;
        req->setBody(Json::writeString(writer, body));

        auto [resp_result, resp] = client->sendRequest(
            req, static_cast<double>(timeout_ms_) / 1000.0);

        if (resp_result != drogon::ReqResult::Ok || !resp) {
            result.valid = false;
            result.error = "Auth service unreachable";
            auth_failures_.fetch_add(1, std::memory_order_relaxed);
            return result;
        }

        if (resp->statusCode() != drogon::k200OK) {
            result.valid = false;
            result.error = "Auth service returned HTTP " +
                           std::to_string(static_cast<int>(resp->statusCode()));
            auth_failures_.fetch_add(1, std::memory_order_relaxed);
            return result;
        }

        auto json = resp->getJsonObject();
        if (!json) {
            result.valid = false;
            result.error = "Auth service returned invalid JSON";
            auth_failures_.fetch_add(1, std::memory_order_relaxed);
            return result;
        }

        result.valid      = (*json).get("valid",   false).asBool();
        result.user_id    = (*json).get("user_id", "").asString();
        result.ttl_seconds = (*json).get("ttl",   cache_ttl_seconds_).asInt();
        result.error      = (*json).get("error",   "").asString();

        if (json->isMember("scopes") && (*json)["scopes"].isArray())
            for (const auto& s : (*json)["scopes"])
                if (s.isString()) result.scopes.push_back(s.asString());

    } catch (const std::exception& e) {
        result.valid = false;
        result.error = std::string("Auth service error: ") + e.what();
        auth_failures_.fetch_add(1, std::memory_order_relaxed);
    }

    return result;
}

// =============================================================================
//  makeUnauthorizedResponse
// =============================================================================
drogon::HttpResponsePtr JwtAuthFilter::makeUnauthorizedResponse(const std::string& error) {
    Json::Value err;
    err["error"]    = "Unauthorized";
    err["message"]  = error;
    err["auth_url"] = auth_url_;
    auto resp = drogon::HttpResponse::newHttpJsonResponse(err);
    resp->setStatusCode(drogon::k401Unauthorized);
    resp->addHeader("WWW-Authenticate", "Bearer");
    return resp;
}

// =============================================================================
//  doFilter — main filter, now uses integer hash key and sharded cache
// =============================================================================
void JwtAuthFilter::doFilter(const drogon::HttpRequestPtr& req,
                              drogon::FilterCallback&& failCb,
                              drogon::FilterChainCallback&& passCb) {
    if (!initialized_.load(std::memory_order_relaxed)) { passCb(); return; }
    if (isExemptPath(req->path()))                     { passCb(); return; }

    std::string token = extractBearerToken(req);
    if (token.empty()) {
        failCb(makeUnauthorizedResponse(
            "Missing Bearer token. Send: Authorization: Bearer <jwt>"));
        return;
    }

    uint64_t hash   = hashToken(token);
    auto     cached = checkCache(hash);

    if (cached.has_value()) {
        cache_hits_.fetch_add(1, std::memory_order_relaxed);
        if (cached->valid) {
            req->addHeader("X-Auth-User", cached->user_id);
            passCb();
        } else {
            failCb(makeUnauthorizedResponse(cached->error));
        }
        return;
    }

    cache_misses_.fetch_add(1, std::memory_order_relaxed);
    JwtValidationResult result = callAuthService(token);
    cacheResult(hash, result);

    if (result.valid) {
        req->addHeader("X-Auth-User", result.user_id);
        passCb();
    } else {
        failCb(makeUnauthorizedResponse(
            result.error.empty() ? "Invalid or expired token" : result.error));
    }
}

} // namespace addr
