#pragma once

#include <string>
#include <atomic>
#include <mutex>
#include <vector>
#include <chrono>
#include <sstream>

namespace addr {

class MetricsCollector {
public:
    static MetricsCollector& instance() {
        static MetricsCollector inst;
        return inst;
    }

    // Record a parse operation
    void recordParse(double latency_ms, bool success);
    void recordBatch(int batch_size, double latency_ms);
    void recordCacheHit();
    void recordCacheMiss();
    void recordLLMFallback(double latency_ms);
    void recordConfidence(double score);
    void recordPhase(int phase);

    // Produce Prometheus text exposition format
    std::string serialize() const;

    // Counters
    std::atomic<uint64_t> total_requests{0};
    std::atomic<uint64_t> total_parse_success{0};
    std::atomic<uint64_t> total_parse_errors{0};
    std::atomic<uint64_t> total_batch_requests{0};
    std::atomic<uint64_t> total_cache_hits{0};
    std::atomic<uint64_t> total_cache_misses{0};
    std::atomic<uint64_t> total_llm_fallbacks{0};
    std::atomic<uint64_t> total_addresses_processed{0};

    // Phase counters
    std::atomic<uint64_t> phase1_count{0};
    std::atomic<uint64_t> phase2_count{0};
    std::atomic<uint64_t> phase3_count{0};
    std::atomic<uint64_t> phase4_count{0};

private:
    MetricsCollector() = default;

    // Histogram buckets for latency
    mutable std::mutex latency_mutex_;
    std::vector<double> parse_latencies_;
    std::vector<double> batch_latencies_;
    std::vector<double> llm_latencies_;
    std::vector<double> confidence_scores_;

    // Histogram bucket boundaries (ms)
    static constexpr double LATENCY_BUCKETS[] = {0.1, 0.25, 0.5, 1.0, 2.5, 5.0, 10.0, 25.0, 50.0, 100.0, 250.0, 500.0, 1000.0};
    static constexpr int NUM_LATENCY_BUCKETS = 13;

    std::string histogramBuckets(const std::string& name, const std::vector<double>& values) const;
};

} // namespace addr
