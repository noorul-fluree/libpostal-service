#include "controllers/BatchController.h"
#include "services/AddressParser.h"
#include "services/PreProcessor.h"
#include "services/ConfidenceScorer.h"
#include "services/RuleEngine.h"
#include "services/CacheManager.h"
#include "services/MetricsCollector.h"
#include "models/AddressModels.h"

extern addr::AddressParser     g_parser;
extern addr::PreProcessor      g_preprocessor;
extern addr::ConfidenceScorer  g_scorer;
extern addr::RuleEngine        g_rules;
extern addr::CacheManager*     g_cache;   // pointer
extern addr::ServiceConfig     g_config;

namespace addr {

void BatchController::batchParse(const drogon::HttpRequestPtr& req,
                                  std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
    ScopedTimer total_timer;
    auto& metrics = MetricsCollector::instance();

    auto json = req->getJsonObject();
    if (!json) {
        auto resp = drogon::HttpResponse::newHttpJsonResponse(Json::Value("Invalid JSON body"));
        resp->setStatusCode(drogon::k400BadRequest);
        callback(resp);
        return;
    }

    BatchRequest batch_req = BatchRequest::fromJson(*json);

    if (batch_req.addresses.empty()) {
        auto resp = drogon::HttpResponse::newHttpJsonResponse(Json::Value("Empty addresses array"));
        resp->setStatusCode(drogon::k400BadRequest);
        callback(resp);
        return;
    }

    if (static_cast<int>(batch_req.addresses.size()) > g_config.batch_max_size) {
        Json::Value err;
        err["error"]          = "Batch size exceeds maximum of " + std::to_string(g_config.batch_max_size);
        err["max_batch_size"] = g_config.batch_max_size;
        err["received"]       = static_cast<int>(batch_req.addresses.size());
        auto resp = drogon::HttpResponse::newHttpJsonResponse(err);
        resp->setStatusCode(drogon::k400BadRequest);
        callback(resp);
        return;
    }

    BatchResponse batch_resp;
    batch_resp.total = static_cast<int>(batch_req.addresses.size());
    batch_resp.results.reserve(batch_req.addresses.size());

    for (const auto& raw_address : batch_req.addresses) {
        ScopedTimer addr_timer;

        // === Check cache ===
        if (g_config.cache_enabled && g_cache) {
            auto cached = g_cache->get(raw_address);
            if (cached.has_value()) {
                auto result = cached.value();
                result.latency_ms = addr_timer.elapsedMs();
                result.from_cache = true;
                batch_resp.results.push_back(std::move(result));
                batch_resp.succeeded++;
                metrics.recordCacheHit();
                continue;
            }
            metrics.recordCacheMiss();
        }

        // === Phase 1: Pre-processing ===
        std::string cleaned = g_preprocessor.process(raw_address);
        metrics.recordPhase(1);

        // === Phase 2: libpostal parse ===
        ParsedAddress result = g_parser.parse(cleaned);
        result.raw_input = raw_address;
        result.pipeline_phases.push_back(1);
        result.pipeline_phases.push_back(2);
        metrics.recordPhase(2);

        if (!result.error.empty()) {
            result.latency_ms = addr_timer.elapsedMs();
            batch_resp.results.push_back(std::move(result));
            batch_resp.failed++;
            continue;
        }

        // === Confidence scoring ===
        result.confidence = g_scorer.score(result);
        metrics.recordConfidence(result.confidence);

        // === Phase 3: Rule engine ===
        if (g_config.rules_enabled && result.confidence < g_config.llm_confidence_threshold) {
            bool improved = g_rules.apply(result);
            if (improved) {
                result.pipeline_phases.push_back(3);
                result.confidence = g_scorer.score(result);
                metrics.recordPhase(3);
            }
        }

        // Phase 4 (LLM) disabled in this build

        result.latency_ms = addr_timer.elapsedMs();

        if (g_config.cache_enabled && g_cache && result.error.empty()) {
            g_cache->put(raw_address, result);
        }

        batch_resp.succeeded++;
        batch_resp.results.push_back(std::move(result));
    }

    batch_resp.total_latency_ms = total_timer.elapsedMs();
    metrics.recordBatch(batch_resp.total, batch_resp.total_latency_ms);

    callback(drogon::HttpResponse::newHttpJsonResponse(batch_resp.toJson()));
}

} // namespace addr