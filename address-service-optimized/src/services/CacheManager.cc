#include "services/CacheManager.h"

namespace addr {

CacheManager::CacheManager(size_t max_entries, int ttl_seconds)
    : max_per_shard_((max_entries + NUM_SHARDS - 1) / NUM_SHARDS)
    , ttl_seconds_(ttl_seconds)
{}

std::optional<ParsedAddress> CacheManager::get(const std::string& address) {
    uint64_t h     = hashKey(address);
    Shard&   shard = shards_[shardIndex(h)];

    std::lock_guard<std::mutex> lock(shard.mutex);

    auto it = shard.map.find(h);
    if (it == shard.map.end()) {
        misses_.fetch_add(1, std::memory_order_relaxed);
        return std::nullopt;
    }

    auto& list_it = it->second;
    if (isExpired(list_it->second)) {
        shard.map.erase(it);
        shard.lru.erase(list_it);
        misses_.fetch_add(1, std::memory_order_relaxed);
        return std::nullopt;
    }

    // Splice to front (O(1) — just pointer rewiring, no copy)
    shard.lru.splice(shard.lru.begin(), shard.lru, list_it);
    hits_.fetch_add(1, std::memory_order_relaxed);

    ParsedAddress result = list_it->second.value;
    result.from_cache = true;
    return result;
}

void CacheManager::put(const std::string& address, const ParsedAddress& result) {
    uint64_t h     = hashKey(address);
    Shard&   shard = shards_[shardIndex(h)];

    std::lock_guard<std::mutex> lock(shard.mutex);

    auto it = shard.map.find(h);
    if (it != shard.map.end()) {
        // Update in place and move to front
        it->second->second.value    = result;
        it->second->second.inserted = std::chrono::steady_clock::now();
        shard.lru.splice(shard.lru.begin(), shard.lru, it->second);
        return;
    }

    // Evict if this shard is full
    while (shard.map.size() >= max_per_shard_) {
        evictOne(shard);
    }

    // Push to front of LRU
    shard.lru.push_front({h, {result, std::chrono::steady_clock::now()}});
    shard.map[h] = shard.lru.begin();
}

void CacheManager::evictOne(Shard& shard) {
    if (shard.lru.empty()) return;
    auto& back = shard.lru.back();
    shard.map.erase(back.first);
    shard.lru.pop_back();
}

double CacheManager::hitRatio() const {
    uint64_t h = hits_.load(std::memory_order_relaxed);
    uint64_t m = misses_.load(std::memory_order_relaxed);
    uint64_t t = h + m;
    return t == 0 ? 0.0 : static_cast<double>(h) / t;
}

size_t CacheManager::size() const {
    size_t total = 0;
    for (const auto& shard : shards_) {
        std::lock_guard<std::mutex> lock(shard.mutex);
        total += shard.map.size();
    }
    return total;
}

void CacheManager::reset() {
    for (auto& shard : shards_) {
        std::lock_guard<std::mutex> lock(shard.mutex);
        shard.lru.clear();
        shard.map.clear();
    }
    hits_.store(0, std::memory_order_relaxed);
    misses_.store(0, std::memory_order_relaxed);
}

} // namespace addr
