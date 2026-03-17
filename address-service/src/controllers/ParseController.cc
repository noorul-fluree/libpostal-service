#include "controllers/ParseController.h"
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
extern addr::CacheManager*     g_cache;   // pointer — mutex not move-assignable
extern addr::ServiceConfig     g_config;

namespace addr {

void ParseController::parse(const drogon::HttpRequestPtr& req,
                             std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
    ScopedTimer timer;
    auto& metrics = MetricsCollector::instance();

    auto json = req->getJsonObject();
    if (!json || !json->isMember("address")) {
        auto resp = drogon::HttpResponse::newHttpJsonResponse(
            Json::Value("Missing 'address' field in request body"));
        resp->setStatusCode(drogon::k400BadRequest);
        callback(resp);
        return;
    }

    std::string raw_address = (*json)["address"].asString();
    if (raw_address.empty()) {
        auto resp = drogon::HttpResponse::newHttpJsonResponse(Json::Value("Empty address"));
        resp->setStatusCode(drogon::k400BadRequest);
        callback(resp);
        return;
    }

    // === Check cache ===
    if (g_config.cache_enabled && g_cache) {
        auto cached = g_cache->get(raw_address);
        if (cached.has_value()) {
            auto result = cached.value();
            result.latency_ms = timer.elapsedMs();
            result.from_cache = true;
            metrics.recordParse(result.latency_ms, true);
            metrics.recordCacheHit();
            callback(drogon::HttpResponse::newHttpJsonResponse(result.toJson()));
            return;
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
        result.latency_ms = timer.elapsedMs();
        metrics.recordParse(result.latency_ms, false);
        auto resp = drogon::HttpResponse::newHttpJsonResponse(result.toJson());
        resp->setStatusCode(drogon::k500InternalServerError);
        callback(resp);
        return;
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

    result.latency_ms = timer.elapsedMs();

    if (g_config.cache_enabled && g_cache && result.error.empty()) {
        g_cache->put(raw_address, result);
    }

    metrics.recordParse(result.latency_ms, result.error.empty());
    callback(drogon::HttpResponse::newHttpJsonResponse(result.toJson()));
}

void ParseController::normalize(const drogon::HttpRequestPtr& req,
                                 std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
    ScopedTimer timer;

    auto json = req->getJsonObject();
    if (!json || !json->isMember("address")) {
        auto resp = drogon::HttpResponse::newHttpJsonResponse(Json::Value("Missing 'address' field"));
        resp->setStatusCode(drogon::k400BadRequest);
        callback(resp);
        return;
    }

    std::string raw     = (*json)["address"].asString();
    std::string cleaned = g_preprocessor.process(raw);

    NormalizedAddress result = g_parser.normalize(cleaned);
    result.latency_ms = timer.elapsedMs();

    callback(drogon::HttpResponse::newHttpJsonResponse(result.toJson()));
}

} // namespace addr