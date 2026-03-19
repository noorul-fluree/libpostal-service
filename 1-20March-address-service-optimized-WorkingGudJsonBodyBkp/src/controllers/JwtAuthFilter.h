#pragma once

#include <drogon/HttpFilter.h>
#include <drogon/HttpClient.h>
#include <string>
#include <unordered_map>
#include <mutex>
#include <chrono>
#include <atomic>
#include <array>
#include <optional>

namespace addr {

struct JwtValidationResult {
    bool        valid       = false;
    std::string user_id;
    std::vector<std::string> scopes;
    int         ttl_seconds = 0;
    std::string error;
    std::chrono::steady_clock::time_point cached_at;
};

// =============================================================================
//  JwtAuthFilter — JWT validation with sharded token cache
//
//  Key optimizations vs original:
//   1. Token hashed to uint64_t (FNV-1a) before cache lookup — integer key,
//      no string compare inside the lock
//   2. Cache sharded into NUM_CACHE_SHARDS independent mutexes — 16x lower
//      contention vs single mutex at high QPS
//   3. isExemptPath uses prefix check, not strstr (branch-free for common paths)
//   4. Bearer token extraction avoids substr by checking chars directly
// =============================================================================
class JwtAuthFilter : public drogon::HttpFilter<JwtAuthFilter> {
public:
    void doFilter(const drogon::HttpRequestPtr& req,
                  drogon::FilterCallback&&      failCb,
                  drogon::FilterChainCallback&& passCb) override;

    static void initialize(const std::string& auth_url,
                           int timeout_ms        = 3000,
                           int cache_ttl_seconds = 300,
                           int cache_max_entries = 10000);

    static bool     isInitialized() { return initialized_.load(); }
    static uint64_t cacheHits()     { return cache_hits_.load(); }
    static uint64_t cacheMisses()   { return cache_misses_.load(); }
    static uint64_t authCalls()     { return auth_calls_.load(); }
    static uint64_t authFailures()  { return auth_failures_.load(); }

    // Exposed for CacheManager-style sharding
    static constexpr size_t NUM_CACHE_SHARDS = 16; // power-of-2

    struct alignas(64) TokenCacheShard {
        mutable std::mutex mutex;
        std::unordered_map<uint64_t, JwtValidationResult> map;
    };

private:
    static std::atomic<bool> initialized_;
    static std::string       auth_url_;
    static int               timeout_ms_;
    static int               cache_ttl_seconds_;
    static int               cache_max_entries_;

    static std::array<TokenCacheShard, NUM_CACHE_SHARDS> cache_shards_;

    static std::atomic<uint64_t> cache_hits_;
    static std::atomic<uint64_t> cache_misses_;
    static std::atomic<uint64_t> auth_calls_;
    static std::atomic<uint64_t> auth_failures_;

    // FNV-1a 64-bit hash (no heap alloc, branch-free)
    static uint64_t hashToken(const std::string& token) noexcept;

    static std::string  extractBearerToken(const drogon::HttpRequestPtr& req);
    static bool         isExemptPath(const std::string& path);

    // Sharded cache ops (integer key, no string copy)
    static std::optional<JwtValidationResult> checkCache(uint64_t token_hash);
    static void cacheResult(uint64_t token_hash, const JwtValidationResult& result);

    static JwtValidationResult callAuthService(const std::string& token);
    static drogon::HttpResponsePtr makeUnauthorizedResponse(const std::string& error);
};

} // namespace addr
