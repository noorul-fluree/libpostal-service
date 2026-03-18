#include "services/MetricsCollector.h"

namespace addr {

void MetricsCollector::recordParse(double latency_ms, bool success) {
    total_requests.fetch_add(1, std::memory_order_relaxed);
    total_addresses_processed.fetch_add(1, std::memory_order_relaxed);
    if (success)
        total_parse_success.fetch_add(1, std::memory_order_relaxed);
    else
        total_parse_errors.fetch_add(1, std::memory_order_relaxed);
    parse_hist_.record(latency_ms);
}

void MetricsCollector::recordBatch(int batch_size, double latency_ms) {
    total_batch_requests.fetch_add(1, std::memory_order_relaxed);
    total_addresses_processed.fetch_add(static_cast<uint64_t>(batch_size),
                                        std::memory_order_relaxed);
    batch_hist_.record(latency_ms);
}

void MetricsCollector::recordLLMFallback(double latency_ms) {
    total_llm_fallbacks.fetch_add(1, std::memory_order_relaxed);
    llm_hist_.record(latency_ms);
}

void MetricsCollector::recordConfidence(double score) {
    // Store confidence as 0-100 ms-equivalent for the histogram
    conf_hist_.record(score * 100.0);
}

void MetricsCollector::recordPhase(int phase) {
    switch (phase) {
        case 1: phase1_count.fetch_add(1, std::memory_order_relaxed); break;
        case 2: phase2_count.fetch_add(1, std::memory_order_relaxed); break;
        case 3: phase3_count.fetch_add(1, std::memory_order_relaxed); break;
        case 4: phase4_count.fetch_add(1, std::memory_order_relaxed); break;
    }
}

std::string MetricsCollector::serializeHistogram(const std::string& name,
                                                   const AtomicHistogram& h) const {
    std::ostringstream oss;
    uint64_t cnt = h.count.load(std::memory_order_relaxed);
    double   sum = static_cast<double>(h.sum_us.load(std::memory_order_relaxed)) / 1000.0;

    for (int i = 0; i < NUM_BUCKETS; ++i) {
        oss << name << "_bucket{le=\"" << LATENCY_BUCKETS[i] << "\"} "
            << h.buckets[i].load(std::memory_order_relaxed) << "\n";
    }
    oss << name << "_bucket{le=\"+Inf\"} " << cnt << "\n";
    oss << name << "_sum "   << sum << "\n";
    oss << name << "_count " << cnt << "\n";
    return oss.str();
}

std::string MetricsCollector::serialize() const {
    std::ostringstream oss;

    oss << "# HELP address_requests_total Total number of requests\n"
        << "# TYPE address_requests_total counter\n"
        << "address_requests_total " << total_requests.load() << "\n\n";

    oss << "# HELP address_parse_success_total Successful parses\n"
        << "# TYPE address_parse_success_total counter\n"
        << "address_parse_success_total " << total_parse_success.load() << "\n\n";

    oss << "# HELP address_parse_errors_total Failed parses\n"
        << "# TYPE address_parse_errors_total counter\n"
        << "address_parse_errors_total " << total_parse_errors.load() << "\n\n";

    oss << "# HELP address_batch_requests_total Batch requests\n"
        << "# TYPE address_batch_requests_total counter\n"
        << "address_batch_requests_total " << total_batch_requests.load() << "\n\n";

    oss << "# HELP address_addresses_processed_total Total addresses processed\n"
        << "# TYPE address_addresses_processed_total counter\n"
        << "address_addresses_processed_total " << total_addresses_processed.load() << "\n\n";

    uint64_t ch = total_cache_hits.load();
    uint64_t cm = total_cache_misses.load();
    double   ratio = (ch + cm > 0) ? static_cast<double>(ch) / (ch + cm) : 0.0;

    oss << "# HELP address_cache_hits_total Cache hits\n"
        << "# TYPE address_cache_hits_total counter\n"
        << "address_cache_hits_total " << ch << "\n\n";

    oss << "# HELP address_cache_hit_ratio Cache hit ratio\n"
        << "# TYPE address_cache_hit_ratio gauge\n"
        << "address_cache_hit_ratio " << ratio << "\n\n";

    oss << "# HELP address_llm_fallback_total LLM fallback invocations\n"
        << "# TYPE address_llm_fallback_total counter\n"
        << "address_llm_fallback_total " << total_llm_fallbacks.load() << "\n\n";

    oss << "# HELP address_pipeline_phase_total Requests per pipeline phase\n"
        << "# TYPE address_pipeline_phase_total counter\n"
        << "address_pipeline_phase_total{phase=\"1\"} " << phase1_count.load() << "\n"
        << "address_pipeline_phase_total{phase=\"2\"} " << phase2_count.load() << "\n"
        << "address_pipeline_phase_total{phase=\"3\"} " << phase3_count.load() << "\n"
        << "address_pipeline_phase_total{phase=\"4\"} " << phase4_count.load() << "\n\n";

    oss << "# HELP address_parse_duration_ms Parse latency in milliseconds\n"
        << "# TYPE address_parse_duration_ms histogram\n"
        << serializeHistogram("address_parse_duration_ms", parse_hist_) << "\n";

    oss << "# HELP address_batch_duration_ms Batch latency in milliseconds\n"
        << "# TYPE address_batch_duration_ms histogram\n"
        << serializeHistogram("address_batch_duration_ms", batch_hist_) << "\n";

    return oss.str();
}

} // namespace addr
