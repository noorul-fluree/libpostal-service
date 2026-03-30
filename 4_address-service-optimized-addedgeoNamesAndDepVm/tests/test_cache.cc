#include "services/CacheManager.h"
#include <iostream>
#include <stdexcept>
#include <functional>
#include <thread>

extern void registerTest(const std::string& name, std::function<void()> fn);

void register_cache_tests() {

    registerTest("Cache::putAndGet", []() {
        addr::CacheManager cache(100, 3600);

        addr::ParsedAddress p;
        p.house_number = "123";
        p.road = "main street";
        p.city = "test city";

        cache.put("123 Main St", p);

        auto result = cache.get("123 Main St");
        if (!result.has_value()) {
            throw std::runtime_error("Cache miss on recently inserted entry");
        }
        if (result->house_number != "123") {
            throw std::runtime_error("Wrong house_number: " + result->house_number);
        }
        if (!result->from_cache) {
            throw std::runtime_error("from_cache should be true");
        }
    });

    registerTest("Cache::miss", []() {
        addr::CacheManager cache(100, 3600);
        auto result = cache.get("nonexistent address");
        if (result.has_value()) {
            throw std::runtime_error("Expected cache miss");
        }
    });

    registerTest("Cache::eviction", []() {
        addr::CacheManager cache(3, 3600); // Max 3 entries

        addr::ParsedAddress p1; p1.city = "city1";
        addr::ParsedAddress p2; p2.city = "city2";
        addr::ParsedAddress p3; p3.city = "city3";
        addr::ParsedAddress p4; p4.city = "city4";

        cache.put("addr1", p1);
        cache.put("addr2", p2);
        cache.put("addr3", p3);
        cache.put("addr4", p4); // Should evict addr1 (LRU)

        auto r1 = cache.get("addr1");
        if (r1.has_value()) {
            throw std::runtime_error("addr1 should have been evicted");
        }

        auto r4 = cache.get("addr4");
        if (!r4.has_value()) {
            throw std::runtime_error("addr4 should be in cache");
        }
        if (r4->city != "city4") {
            throw std::runtime_error("Wrong city for addr4: " + r4->city);
        }
    });

    registerTest("Cache::hitRatio", []() {
        addr::CacheManager cache(100, 3600);

        addr::ParsedAddress p; p.city = "test";
        cache.put("addr1", p);

        cache.get("addr1"); // hit
        cache.get("addr1"); // hit
        cache.get("addr2"); // miss

        double ratio = cache.hitRatio();
        // 2 hits out of 3 total = ~0.667
        if (ratio < 0.6 || ratio > 0.7) {
            throw std::runtime_error("Expected ~0.667 hit ratio, got: " + std::to_string(ratio));
        }
    });

    registerTest("Cache::update", []() {
        addr::CacheManager cache(100, 3600);

        addr::ParsedAddress p1; p1.city = "old_city";
        cache.put("addr1", p1);

        addr::ParsedAddress p2; p2.city = "new_city";
        cache.put("addr1", p2); // Update

        auto result = cache.get("addr1");
        if (!result.has_value()) {
            throw std::runtime_error("Expected cache hit after update");
        }
        if (result->city != "new_city") {
            throw std::runtime_error("Expected updated city, got: " + result->city);
        }
    });

    registerTest("Cache::size", []() {
        addr::CacheManager cache(100, 3600);

        if (cache.size() != 0) {
            throw std::runtime_error("New cache should be empty");
        }

        addr::ParsedAddress p; p.city = "test";
        cache.put("addr1", p);
        cache.put("addr2", p);

        if (cache.size() != 2) {
            throw std::runtime_error("Expected size 2, got: " + std::to_string(cache.size()));
        }
    });

    registerTest("Cache::reset", []() {
        addr::CacheManager cache(100, 3600);

        addr::ParsedAddress p; p.city = "test";
        cache.put("addr1", p);
        cache.put("addr2", p);

        cache.reset();

        if (cache.size() != 0) {
            throw std::runtime_error("Cache should be empty after reset");
        }
        if (cache.hits() != 0 || cache.misses() != 0) {
            throw std::runtime_error("Stats should be reset");
        }
    });

    registerTest("Cache::threadSafety", []() {
        addr::CacheManager cache(10000, 3600);
        std::atomic<int> errors{0};

        auto writer = [&](int start) {
            for (int i = start; i < start + 500; ++i) {
                addr::ParsedAddress p;
                p.city = "city" + std::to_string(i);
                cache.put("addr" + std::to_string(i), p);
            }
        };

        auto reader = [&](int start) {
            for (int i = start; i < start + 500; ++i) {
                cache.get("addr" + std::to_string(i));
            }
        };

        // Run concurrent readers and writers
        std::vector<std::thread> threads;
        threads.emplace_back(writer, 0);
        threads.emplace_back(writer, 500);
        threads.emplace_back(reader, 0);
        threads.emplace_back(reader, 250);

        for (auto& t : threads) t.join();

        if (errors.load() > 0) {
            throw std::runtime_error("Thread safety errors: " + std::to_string(errors.load()));
        }
        // If we get here without crash/deadlock, thread safety is OK
    });
}
