#include "controllers/BatchController.h"
#include "services/AddressParser.h"
#include "services/PreProcessor.h"
#include "services/ConfidenceScorer.h"
#include "services/RuleEngine.h"
#include "services/CacheManager.h"
#include "services/LLMFallback.h"
#include "services/MetricsCollector.h"
#include "models/AddressModels.h"
#include "utils/InputValidator.h"
#include <iostream>
#include <future>
#include <vector>

extern addr::AddressParser     g_parser;
extern addr::PreProcessor      g_preprocessor;
extern addr::ConfidenceScorer  g_scorer;
extern addr::RuleEngine        g_rules;
extern addr::CacheManager*     g_cache;
extern addr::LLMFallback       g_llm;
extern addr::ServiceConfig     g_config;

namespace addr {

static ParsedAddress processOneAddress(const std::string& raw_address,
                                        const std::string& language,
                                        const std::string& country) {
    ScopedTimer addr_timer;
    auto& metrics = MetricsCollector::instance();

    auto addr_validation = InputValidator::validateAddress(raw_address);
    if (!addr_validation.valid) {
        ParsedAddress err;
        err.raw_input  = raw_address;
        err.error      = addr_validation.error;
        err.latency_ms = addr_timer.elapsedMs();
        return err;
    }

    std::string sanitized = InputValidator::sanitize(raw_address);

    if (g_config.cache_enabled) {
        auto cached = g_cache->get(sanitized);
        if (cached.has_value()) {
            cached->latency_ms = addr_timer.elapsedMs();
            cached->from_cache = true;
            metrics.recordCacheHit();
            return *cached;
        }
        metrics.recordCacheMiss();
    }

    std::string cleaned = g_preprocessor.process(sanitized);
    metrics.recordPhase(1);

    const std::string& lang = language.empty() ? g_config.default_language : language;
    const std::string& ctry = country.empty()  ? g_config.default_country  : country;

    ParsedAddress result = g_parser.parse(cleaned, lang, ctry);
    result.raw_input = sanitized;
    result.pipeline_phases.push_back(1);
    result.pipeline_phases.push_back(2);
    metrics.recordPhase(2);

    if (!result.error.empty()) {
        result.latency_ms = addr_timer.elapsedMs();
        return result;
    }

    result.confidence = g_scorer.score(result);
    metrics.recordConfidence(result.confidence);

    if (g_config.rules_enabled && result.confidence < g_config.llm_confidence_threshold) {
        bool improved = g_rules.apply(result);
        if (improved) {
            result.pipeline_phases.push_back(3);
            result.confidence = g_scorer.score(result);
            metrics.recordPhase(3);
        }
    }

    if (g_config.llm_enabled && g_llm.isReady() &&
        result.confidence < g_config.llm_low_threshold) {
        ScopedTimer llm_t;
        bool improved = g_llm.improve(result, g_config.llm_timeout_ms);
        if (improved) {
            result.pipeline_phases.push_back(4);
            result.confidence = g_scorer.score(result);
        }
        metrics.recordLLMFallback(llm_t.elapsedMs());
        metrics.recordPhase(4);
    }

    result.latency_ms = addr_timer.elapsedMs();

    if (g_config.cache_enabled && result.error.empty() &&
        result.confidence >= g_config.cache_min_confidence)
        g_cache->put(sanitized, result);

    return result;
}

static constexpr size_t SERIAL_THRESHOLD = 8;

void BatchController::batchParse(const drogon::HttpRequestPtr& req,
                                  std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
    ScopedTimer total_timer;
    auto& metrics = MetricsCollector::instance();

    auto json = req->getJsonObject();
    if (!json) {
        std::cout << "[BatchController] ✘ POST /api/v1/batch | error=Invalid JSON body\n";
        auto r = drogon::HttpResponse::newHttpJsonResponse(Json::Value("Invalid JSON body"));
        r->setStatusCode(drogon::k400BadRequest);
        callback(r); return;
    }

    auto batch_validation = InputValidator::validateBatch(*json, g_config.batch_max_size);
    if (!batch_validation.valid) {
        std::cout << "[BatchController] ✘ POST /api/v1/batch | error=" << batch_validation.error << "\n";
        Json::Value err; err["error"] = batch_validation.error;
        auto r = drogon::HttpResponse::newHttpJsonResponse(err);
        r->setStatusCode(static_cast<drogon::HttpStatusCode>(batch_validation.status_code));
        callback(r); return;
    }

    BatchRequest batch_req = BatchRequest::fromJson(*json);
    const auto&  addresses = batch_req.addresses;
    const size_t N         = addresses.size();

    std::cout << "[BatchController] ▶ POST /api/v1/batch | records_in=" << N
              << " | max_allowed=" << g_config.batch_max_size << "\n";

    const std::string& lang = batch_req.default_language.empty()
                              ? g_config.default_language : batch_req.default_language;
    const std::string& ctry = batch_req.default_country.empty()
                              ? g_config.default_country  : batch_req.default_country;

    BatchResponse batch_resp;
    batch_resp.total = static_cast<int>(N);
    batch_resp.results.resize(N);

    // Step 1: validate + sanitize
    std::vector<std::string> sanitized(N);
    std::vector<bool>        is_invalid(N, false);

    for (size_t i = 0; i < N; ++i) {
        auto v = InputValidator::validateAddress(addresses[i]);
        if (!v.valid) {
            batch_resp.results[i].raw_input  = addresses[i];
            batch_resp.results[i].error      = v.error;
            batch_resp.results[i].latency_ms = 0.0;
            is_invalid[i] = true;
            ++batch_resp.failed;
        } else {
            sanitized[i] = InputValidator::sanitize(addresses[i]);
        }
    }

    // Step 2: dedup
    uint64_t hits_before   = metrics.total_cache_hits.load();
    uint64_t misses_before = metrics.total_cache_misses.load();

    auto dedup = g_parser.deduplicateBatch(sanitized);

    // Step 3: cache check
    std::vector<bool> parse_needed(N, false);
    for (size_t idx : dedup.unique_indices) {
        if (is_invalid[idx]) continue;
        if (g_config.cache_enabled) {
            auto cached = g_cache->get(sanitized[idx]);
            if (cached.has_value()) {
                cached->from_cache = true;
                batch_resp.results[idx] = std::move(*cached);
                ++batch_resp.succeeded;
                metrics.recordCacheHit();
                continue;
            }
            metrics.recordCacheMiss();
        }
        parse_needed[idx] = true;
    }

    std::vector<size_t> to_parse;
    to_parse.reserve(dedup.unique_indices.size());
    for (size_t idx : dedup.unique_indices)
        if (parse_needed[idx]) to_parse.push_back(idx);

    // Step 4: parallel parse
    if (to_parse.size() <= SERIAL_THRESHOLD) {
        for (size_t idx : to_parse)
            batch_resp.results[idx] = processOneAddress(sanitized[idx], lang, ctry);
    } else {
        std::vector<std::future<ParsedAddress>> futures;
        futures.reserve(to_parse.size());
        for (size_t idx : to_parse)
            futures.push_back(std::async(std::launch::async,
                processOneAddress, sanitized[idx], lang, ctry));
        for (size_t fi = 0; fi < to_parse.size(); ++fi)
            batch_resp.results[to_parse[fi]] = futures[fi].get();
    }

    // Step 5: fan out duplicates
    for (size_t i = 0; i < N; ++i) {
        if (is_invalid[i]) continue;
        int canonical = dedup.canonical_index[i];
        if (canonical >= 0 && static_cast<size_t>(canonical) != i) {
            batch_resp.results[i]            = batch_resp.results[canonical];
            batch_resp.results[i].raw_input  = addresses[i];
            batch_resp.results[i].from_cache = true;
        }
        if (batch_resp.results[i].error.empty()) ++batch_resp.succeeded;
        else                                     ++batch_resp.failed;
    }

    batch_resp.total_latency_ms = total_timer.elapsedMs();

    uint64_t hits   = metrics.total_cache_hits.load()   - hits_before;
    uint64_t misses = metrics.total_cache_misses.load() - misses_before;

    std::cout << "[BatchController] ✔ POST /api/v1/batch"
              << " | records_in="   << N
              << " | succeeded="    << batch_resp.succeeded
              << " | failed="       << batch_resp.failed
              << " | unique_parsed="<< to_parse.size()
              << " | cache_hits="   << hits
              << " | cache_misses=" << misses
              << " | latency="      << batch_resp.total_latency_ms << "ms\n";

    if (batch_resp.failed > 0)
        std::cout << "[BatchController]   ✘ " << batch_resp.failed << " record(s) failed validation\n";

    metrics.recordBatch(batch_resp.total, batch_resp.total_latency_ms);
    auto resp = drogon::HttpResponse::newHttpJsonResponse(batch_resp.toJson());
    callback(resp);
}

} // namespace addr