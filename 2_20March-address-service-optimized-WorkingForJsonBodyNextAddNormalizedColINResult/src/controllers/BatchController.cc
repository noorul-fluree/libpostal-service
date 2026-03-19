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

// =============================================================================
//  processOneAddress
//  FIX 3: now accepts language/country hints and passes them to libpostal
//  FIX 1: thread safety is handled inside AddressParser::parse() via mutex pool
// =============================================================================
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

    // Cache check (sharded — cheap)
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

    // Phase 1: pre-process
    std::string cleaned = g_preprocessor.process(sanitized);
    metrics.recordPhase(1);

    // Phase 2: libpostal parse — FIX 3: pass language/country hints
    // These come from the batch request or fall back to service config defaults.
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

    // Phase 3: rule engine
    if (g_config.rules_enabled && result.confidence < g_config.llm_confidence_threshold) {
        bool improved = g_rules.apply(result);
        if (improved) {
            result.pipeline_phases.push_back(3);
            result.confidence = g_scorer.score(result);
            metrics.recordPhase(3);
        }
    }

    // Phase 4: LLM fallback
    // g_llm.isReady() returns false when DISABLE_LLM=true is set,
    // so this entire block is skipped — zero overhead on the core path.
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
        result.confidence >= g_config.cache_min_confidence) {
        g_cache->put(sanitized, result);
    }

    return result;
}

// =============================================================================
//  batchParse
//
//  Pipeline:
//    1. Validate + sanitize all addresses
//    2. FIX 6: near-dupe dedup — collapse duplicates before parsing
//    3. Cache-check all unique addresses
//    4. Parallel parse only the cache-miss uniques
//    5. Fan results back out to duplicates
//
//  This means a batch of 2000 addresses where 300 are near-dups of each other
//  triggers at most 1700 libpostal calls instead of 2000 — and usually far
//  fewer after the cache is warm.
// =============================================================================
static constexpr size_t SERIAL_THRESHOLD = 8;

void BatchController::batchParse(const drogon::HttpRequestPtr& req,
                                  std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
    ScopedTimer total_timer;
    auto& metrics = MetricsCollector::instance();

    auto json = req->getJsonObject();
    if (!json) {
        auto r = drogon::HttpResponse::newHttpJsonResponse(Json::Value("Invalid JSON body"));
        r->setStatusCode(drogon::k400BadRequest);
        callback(r);
        return;
    }

    auto batch_validation = InputValidator::validateBatch(*json, g_config.batch_max_size);
    if (!batch_validation.valid) {
        Json::Value err;
        err["error"] = batch_validation.error;
        auto r = drogon::HttpResponse::newHttpJsonResponse(err);
        r->setStatusCode(static_cast<drogon::HttpStatusCode>(batch_validation.status_code));
        callback(r);
        return;
    }

    BatchRequest batch_req = BatchRequest::fromJson(*json);
    const auto&  addresses = batch_req.addresses;
    const size_t N         = addresses.size();

    // Resolve language/country: per-request hint > config default
    const std::string& lang = batch_req.default_language.empty()
                              ? g_config.default_language : batch_req.default_language;
    const std::string& ctry = batch_req.default_country.empty()
                              ? g_config.default_country  : batch_req.default_country;

    BatchResponse batch_resp;
    batch_resp.total = static_cast<int>(N);
    batch_resp.results.resize(N);

    // ==================================================================
    //  Step 1: validate + sanitize all addresses upfront
    // ==================================================================
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

    // ==================================================================
    //  Step 2: near-dupe deduplication (FIX 6)
    //  Build list of sanitized addresses for dedup (skip invalids)
    // ==================================================================
    auto dedup = g_parser.deduplicateBatch(sanitized);
    // dedup.unique_indices = indices we actually need to parse
    // dedup.canonical_index[i] = which index's result to copy for address i

    // ==================================================================
    //  Step 3: cache-check all unique addresses, collect cache misses
    // ==================================================================
    // parse_needed[i] = true if unique address i still needs libpostal
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

    // Build the list of indices that genuinely need parsing
    std::vector<size_t> to_parse;
    to_parse.reserve(dedup.unique_indices.size());
    for (size_t idx : dedup.unique_indices) {
        if (parse_needed[idx]) to_parse.push_back(idx);
    }

    // ==================================================================
    //  Step 4: parallel parse of cache-miss unique addresses
    // ==================================================================
    if (to_parse.size() <= SERIAL_THRESHOLD) {
        for (size_t idx : to_parse) {
            batch_resp.results[idx] = processOneAddress(sanitized[idx], lang, ctry);
        }
    } else {
        std::vector<std::future<ParsedAddress>> futures;
        futures.reserve(to_parse.size());
        for (size_t idx : to_parse) {
            futures.push_back(std::async(std::launch::async,
                processOneAddress, sanitized[idx], lang, ctry));
        }
        for (size_t fi = 0; fi < to_parse.size(); ++fi) {
            batch_resp.results[to_parse[fi]] = futures[fi].get();
        }
    }

    // ==================================================================
    //  Step 5: fan results from canonicals back out to their duplicates
    // ==================================================================
    for (size_t i = 0; i < N; ++i) {
        if (is_invalid[i]) continue;

        int canonical = dedup.canonical_index[i];
        if (canonical >= 0 && static_cast<size_t>(canonical) != i) {
            // Copy result from canonical, mark as deduped cache hit
            batch_resp.results[i]            = batch_resp.results[canonical];
            batch_resp.results[i].raw_input  = addresses[i];
            batch_resp.results[i].from_cache = true;
        }

        if (batch_resp.results[i].error.empty()) ++batch_resp.succeeded;
        else                                     ++batch_resp.failed;
    }

    batch_resp.total_latency_ms = total_timer.elapsedMs();
    metrics.recordBatch(batch_resp.total, batch_resp.total_latency_ms);

    auto resp = drogon::HttpResponse::newHttpJsonResponse(batch_resp.toJson());
    callback(resp);
}

} // namespace addr
