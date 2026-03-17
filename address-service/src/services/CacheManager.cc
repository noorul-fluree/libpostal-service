#include "services/CacheManager.h"
#include <functional>

namespace addr {

CacheManager::CacheManager(size_t max_entries, int ttl_seconds)
    : max_entries_(max_entries), ttl_seconds_(ttl_seconds) {}

std::string CacheManager::makeKey(const std::string& address) {
    // Use std::hash for fast key generation
    size_t h = std::hash<std::string>{}(address);
    return std::to_string(h);
}

std::optional<ParsedAddress> CacheManager::get(const std::string& address) {
    std::string key = makeKey(address);
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = map_.find(key);
    if (it == map_.end()) {
        misses_.fetch_add(1, std::memory_order_relaxed);
        return std::nullopt;
    }

    // Check TTL
    if (isExpired(*it->second)) {
        lru_list_.erase(it->second);
        map_.erase(it);
        misses_.fetch_add(1, std::memory_order_relaxed);
        return std::nullopt;
    }

    // Move to front (most recently used)
    lru_list_.splice(lru_list_.begin(), lru_list_, it->second);
    hits_.fetch_add(1, std::memory_order_relaxed);

    ParsedAddress result = it->second->value;
    result.from_cache = true;
    return result;
}

void CacheManager::put(const std::string& address, const ParsedAddress& result) {
    std::string key = makeKey(address);
    std::lock_guard<std::mutex> lock(mutex_);

    // Check if key already exists
    auto it = map_.find(key);
    if (it != map_.end()) {
        // Update existing entry and move to front
        it->second->value = result;
        it->second->inserted = std::chrono::steady_clock::now();
        lru_list_.splice(lru_list_.begin(), lru_list_, it->second);
        return;
    }

    // Evict if at capacity
    while (map_.size() >= max_entries_) {
        evict();
    }

    // Insert new entry at front
    CacheEntry entry;
    entry.key = key;
    entry.value = result;
    entry.inserted = std::chrono::steady_clock::now();

    lru_list_.push_front(entry);
    map_[key] = lru_list_.begin();
}

void CacheManager::evict() {
    if (lru_list_.empty()) return;
    auto& back = lru_list_.back();
    map_.erase(back.key);
    lru_list_.pop_back();
}

bool CacheManager::isExpired(const CacheEntry& entry) const {
    auto now = std::chrono::steady_clock::now();
    auto age = std::chrono::duration_cast<std::chrono::seconds>(now - entry.inserted);
    return age.count() > ttl_seconds_;
}

double CacheManager::hitRatio() const {
    uint64_t h = hits_.load();
    uint64_t m = misses_.load();
    uint64_t total = h + m;
    if (total == 0) return 0.0;
    return static_cast<double>(h) / total;
}

size_t CacheManager::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return map_.size();
}

void CacheManager::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    lru_list_.clear();
    map_.clear();
    hits_.store(0);
    misses_.store(0);
}

} // namespace addr
