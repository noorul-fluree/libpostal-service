#include "services/MetricsCollector.h"
#include <cmath>
#include <numeric>
#include <algorithm>

namespace addr {

constexpr double MetricsCollector::LATENCY_BUCKETS[];

void MetricsCollector::recordParse(double latency_ms, bool success) {
    total_requests.fetch_add(1, std::memory_order_relaxed);
    total_addresses_processed.fetch_add(1, std::memory_order_relaxed);
    if (success) {
        total_parse_success.fetch_add(1, std::memory_order_relaxed);
    } else {
        total_parse_errors.fetch_add(1, std::memory_order_relaxed);
    }
    std::lock_guard<std::mutex> lock(latency_mutex_);
    parse_latencies_.push_back(latency_ms);
    // Keep bounded
    if (parse_latencies_.size() > 100000) {
        parse_latencies_.erase(parse_latencies_.begin(), parse_latencies_.begin() + 50000);
    }
}

void MetricsCollector::recordBatch(int batch_size, double latency_ms) {
    total_batch_requests.fetch_add(1, std::memory_order_relaxed);
    total_addresses_processed.fetch_add(batch_size, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(latency_mutex_);
    batch_latencies_.push_back(latency_ms);
    if (batch_latencies_.size() > 10000) {
        batch_latencies_.erase(batch_latencies_.begin(), batch_latencies_.begin() + 5000);
    }
}

void MetricsCollector::recordCacheHit() {
    total_cache_hits.fetch_add(1, std::memory_order_relaxed);
}

void MetricsCollector::recordCacheMiss() {
    total_cache_misses.fetch_add(1, std::memory_order_relaxed);
}

void MetricsCollector::recordLLMFallback(double latency_ms) {
    total_llm_fallbacks.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(latency_mutex_);
    llm_latencies_.push_back(latency_ms);
}

void MetricsCollector::recordConfidence(double score) {
    std::lock_guard<std::mutex> lock(latency_mutex_);
    confidence_scores_.push_back(score);
    if (confidence_scores_.size() > 100000) {
        confidence_scores_.erase(confidence_scores_.begin(), confidence_scores_.begin() + 50000);
    }
}

void MetricsCollector::recordPhase(int phase) {
    switch (phase) {
        case 1: phase1_count.fetch_add(1, std::memory_order_relaxed); break;
        case 2: phase2_count.fetch_add(1, std::memory_order_relaxed); break;
        case 3: phase3_count.fetch_add(1, std::memory_order_relaxed); break;
        case 4: phase4_count.fetch_add(1, std::memory_order_relaxed); break;
    }
}

std::string MetricsCollector::histogramBuckets(const std::string& name,
                                                const std::vector<double>& values) const {
    std::ostringstream oss;
    double sum = 0;
    for (double v : values) sum += v;

    for (int i = 0; i < NUM_LATENCY_BUCKETS; ++i) {
        int count = 0;
        for (double v : values) {
            if (v <= LATENCY_BUCKETS[i]) ++count;
        }
        oss << name << "_bucket{le=\"" << LATENCY_BUCKETS[i] << "\"} " << count << "\n";
    }
    oss << name << "_bucket{le=\"+Inf\"} " << values.size() << "\n";
    oss << name << "_sum " << sum << "\n";
    oss << name << "_count " << values.size() << "\n";
    return oss.str();
}

std::string MetricsCollector::serialize() const {
    std::ostringstream oss;

    // Counters
    oss << "# HELP address_requests_total Total number of requests\n";
    oss << "# TYPE address_requests_total counter\n";
    oss << "address_requests_total " << total_requests.load() << "\n\n";

    oss << "# HELP address_parse_success_total Successful parses\n";
    oss << "# TYPE address_parse_success_total counter\n";
    oss << "address_parse_success_total " << total_parse_success.load() << "\n\n";

    oss << "# HELP address_parse_errors_total Failed parses\n";
    oss << "# TYPE address_parse_errors_total counter\n";
    oss << "address_parse_errors_total " << total_parse_errors.load() << "\n\n";

    oss << "# HELP address_batch_requests_total Batch requests\n";
    oss << "# TYPE address_batch_requests_total counter\n";
    oss << "address_batch_requests_total " << total_batch_requests.load() << "\n\n";

    oss << "# HELP address_addresses_processed_total Total addresses processed\n";
    oss << "# TYPE address_addresses_processed_total counter\n";
    oss << "address_addresses_processed_total " << total_addresses_processed.load() << "\n\n";

    // Cache
    uint64_t ch = total_cache_hits.load();
    uint64_t cm = total_cache_misses.load();
    double ratio = (ch + cm > 0) ? static_cast<double>(ch) / (ch + cm) : 0.0;

    oss << "# HELP address_cache_hits_total Cache hits\n";
    oss << "# TYPE address_cache_hits_total counter\n";
    oss << "address_cache_hits_total " << ch << "\n\n";

    oss << "# HELP address_cache_hit_ratio Cache hit ratio\n";
    oss << "# TYPE address_cache_hit_ratio gauge\n";
    oss << "address_cache_hit_ratio " << ratio << "\n\n";

    // LLM
    oss << "# HELP address_llm_fallback_total LLM fallback invocations\n";
    oss << "# TYPE address_llm_fallback_total counter\n";
    oss << "address_llm_fallback_total " << total_llm_fallbacks.load() << "\n\n";

    // Pipeline phase distribution
    oss << "# HELP address_pipeline_phase_total Requests per pipeline phase\n";
    oss << "# TYPE address_pipeline_phase_total counter\n";
    oss << "address_pipeline_phase_total{phase=\"1\"} " << phase1_count.load() << "\n";
    oss << "address_pipeline_phase_total{phase=\"2\"} " << phase2_count.load() << "\n";
    oss << "address_pipeline_phase_total{phase=\"3\"} " << phase3_count.load() << "\n";
    oss << "address_pipeline_phase_total{phase=\"4\"} " << phase4_count.load() << "\n\n";

    // Histograms
    {
        std::lock_guard<std::mutex> lock(latency_mutex_);

        oss << "# HELP address_parse_duration_ms Parse latency in milliseconds\n";
        oss << "# TYPE address_parse_duration_ms histogram\n";
        oss << histogramBuckets("address_parse_duration_ms", parse_latencies_) << "\n";

        oss << "# HELP address_batch_duration_ms Batch latency in milliseconds\n";
        oss << "# TYPE address_batch_duration_ms histogram\n";
        oss << histogramBuckets("address_batch_duration_ms", batch_latencies_) << "\n";
    }

    return oss.str();
}

} // namespace addr
