#pragma once

#include <string>
#include <atomic>
#include <mutex>
#include <vector>
#include <chrono>
#include <sstream>
#include <array>

namespace addr {

// =============================================================================
//  MetricsCollector — lock-free counters + bounded circular histogram
//
//  Key optimizations vs original:
//   1. All counters are std::atomic — zero locking for increment operations
//   2. Histograms use pre-bucketed atomic counters — no vector, no mutex
//   3. Latency recorded by incrementing the right bucket only (O(1), lock-free)
//   4. sum/count tracked as atomics — no need to store raw samples
//   5. serialize() reads atomics with relaxed ordering — fast, consistent enough
// =============================================================================

class MetricsCollector {
public:
    static MetricsCollector& instance() {
        static MetricsCollector inst;
        return inst;
    }

    void recordParse(double latency_ms, bool success);
    void recordBatch(int batch_size, double latency_ms);
    void recordCacheHit()  { total_cache_hits.fetch_add(1, std::memory_order_relaxed); }
    void recordCacheMiss() { total_cache_misses.fetch_add(1, std::memory_order_relaxed); }
    void recordLLMFallback(double latency_ms);
    void recordConfidence(double score);
    void recordPhase(int phase);

    std::string serialize() const;

    // Counters — all atomic, no lock needed
    std::atomic<uint64_t> total_requests{0};
    std::atomic<uint64_t> total_parse_success{0};
    std::atomic<uint64_t> total_parse_errors{0};
    std::atomic<uint64_t> total_batch_requests{0};
    std::atomic<uint64_t> total_cache_hits{0};
    std::atomic<uint64_t> total_cache_misses{0};
    std::atomic<uint64_t> total_llm_fallbacks{0};
    std::atomic<uint64_t> total_addresses_processed{0};

    std::atomic<uint64_t> phase1_count{0};
    std::atomic<uint64_t> phase2_count{0};
    std::atomic<uint64_t> phase3_count{0};
    std::atomic<uint64_t> phase4_count{0};

private:
    MetricsCollector() = default;

    // Latency bucket boundaries (ms) — must match LATENCY_BUCKETS in .cc
    static constexpr int NUM_BUCKETS = 13;
    // static constexpr double LATENCY_BUCKETS[NUM_BUCKETS];
    static constexpr double LATENCY_BUCKETS[NUM_BUCKETS] = {
    0.1, 0.25, 0.5, 1.0, 2.5, 5.0, 10.0, 25.0, 50.0, 100.0, 250.0, 500.0, 1000.0
};

    // Pre-bucketed atomic histograms — no mutex, no vector
    struct AtomicHistogram {
        std::array<std::atomic<uint64_t>, 13> buckets{};
        std::atomic<uint64_t> count{0};
        // sum stored as fixed-point (microseconds) to avoid float atomics
        std::atomic<uint64_t> sum_us{0};

        void record(double ms) {
            count.fetch_add(1, std::memory_order_relaxed);
            sum_us.fetch_add(static_cast<uint64_t>(ms * 1000.0), std::memory_order_relaxed);
            // Increment all buckets where ms <= bucket boundary (cumulative histogram)
            for (int i = 0; i < 13; ++i) {
                if (ms <= LATENCY_BUCKETS[i]) {
                    buckets[i].fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
    };

    AtomicHistogram parse_hist_;
    AtomicHistogram batch_hist_;
    AtomicHistogram llm_hist_;
    AtomicHistogram conf_hist_; // confidence scores (0.0-1.0, treated as ms*100 for bucketing)

    std::string serializeHistogram(const std::string& name,
                                   const AtomicHistogram& h) const;
};

} // namespace addr
