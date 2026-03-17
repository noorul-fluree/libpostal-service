#pragma once

#include "models/AddressModels.h"
#include <string>
#include <unordered_map>
#include <list>
#include <mutex>
#include <atomic>
#include <chrono>
#include <optional>
#include <functional>

namespace addr {

class CacheManager {
public:
    CacheManager(size_t max_entries = 5000000, int ttl_seconds = 86400);

    // Get a cached result (returns nullopt if miss)
    std::optional<ParsedAddress> get(const std::string& address);

    // Store a result in cache
    void put(const std::string& address, const ParsedAddress& result);

    // Stats
    double hitRatio() const;
    size_t size() const;
    uint64_t hits() const { return hits_.load(); }
    uint64_t misses() const { return misses_.load(); }
    void reset();

private:
    struct CacheEntry {
        std::string key;
        ParsedAddress value;
        std::chrono::steady_clock::time_point inserted;
    };

    using ListIterator = std::list<CacheEntry>::iterator;

    size_t max_entries_;
    int ttl_seconds_;

    mutable std::mutex mutex_;
    std::list<CacheEntry> lru_list_;
    std::unordered_map<std::string, ListIterator> map_;

    std::atomic<uint64_t> hits_{0};
    std::atomic<uint64_t> misses_{0};

    void evict();
    bool isExpired(const CacheEntry& entry) const;

    // Hash function for cache key
    static std::string makeKey(const std::string& address);
};

} // namespace addr
