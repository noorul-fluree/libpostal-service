#include "controllers/ParseController.h"
#include "services/AddressParser.h"
#include "services/PreProcessor.h"
#include "services/ConfidenceScorer.h"
#include "services/RuleEngine.h"
#include "services/CacheManager.h"
#include "services/LLMFallback.h"
#include "services/MetricsCollector.h"
#include "models/AddressModels.h"
#include "utils/InputValidator.h"

extern addr::AddressParser     g_parser;
extern addr::PreProcessor      g_preprocessor;
extern addr::ConfidenceScorer  g_scorer;
extern addr::RuleEngine        g_rules;
extern addr::CacheManager*     g_cache;
extern addr::LLMFallback       g_llm;
extern addr::ServiceConfig     g_config;

namespace addr {

// =============================================================================
//  parse — single address endpoint
//  FIX 3: reads optional "language" and "country" fields from request body
//         and passes them to libpostal for better accuracy.
//
//  Request body:
//    { "address": "123 MG Road, Bengaluru 560001",
//      "language": "en",   // optional ISO 639-1
//      "country":  "in"    // optional ISO 3166-1 alpha-2
//    }
// =============================================================================
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

    // FIX 3: extract optional language/country hints from request body.
    // Per-request hints override the service-level config defaults.
    std::string language = json->isMember("language")
                           ? (*json)["language"].asString()
                           : g_config.default_language;
    std::string country  = json->isMember("country")
                           ? (*json)["country"].asString()
                           : g_config.default_country;

    auto validation = InputValidator::validateAddress(raw_address);
    if (!validation.valid) {
        Json::Value err;
        err["error"]        = validation.error;
        err["input_length"] = static_cast<int>(raw_address.size());
        auto resp = drogon::HttpResponse::newHttpJsonResponse(err);
        resp->setStatusCode(static_cast<drogon::HttpStatusCode>(validation.status_code));
        callback(resp);
        return;
    }

    raw_address = InputValidator::sanitize(raw_address);

    // Cache check
    if (g_config.cache_enabled) {
        auto cached = g_cache->get(raw_address);
        if (cached.has_value()) {
            cached->latency_ms = timer.elapsedMs();
            cached->from_cache = true;
            metrics.recordParse(cached->latency_ms, true);
            metrics.recordCacheHit();
            callback(drogon::HttpResponse::newHttpJsonResponse(cached->toJson()));
            return;
        }
        metrics.recordCacheMiss();
    }

    // Phase 1: pre-process
    std::string cleaned = g_preprocessor.process(raw_address);
    metrics.recordPhase(1);

    // Phase 2: libpostal parse — FIX 3: language + country hints passed
    ParsedAddress result = g_parser.parse(cleaned, language, country);
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

    result.latency_ms = timer.elapsedMs();

    if (g_config.cache_enabled && result.error.empty() &&
        result.confidence >= g_config.cache_min_confidence) {
        g_cache->put(raw_address, result);
    }

    metrics.recordParse(result.latency_ms, result.error.empty());
    callback(drogon::HttpResponse::newHttpJsonResponse(result.toJson()));
}

// =============================================================================
//  normalize — FIX 3: language hint threaded through to libpostal
// =============================================================================
void ParseController::normalize(const drogon::HttpRequestPtr& req,
                                 std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
    ScopedTimer timer;

    auto json = req->getJsonObject();
    if (!json || !json->isMember("address")) {
        auto resp = drogon::HttpResponse::newHttpJsonResponse(
            Json::Value("Missing 'address' field"));
        resp->setStatusCode(drogon::k400BadRequest);
        callback(resp);
        return;
    }

    std::string raw = (*json)["address"].asString();

    std::string language = json->isMember("language")
                           ? (*json)["language"].asString()
                           : g_config.default_language;
    std::string country  = json->isMember("country")
                           ? (*json)["country"].asString()
                           : g_config.default_country;

    auto validation = InputValidator::validateAddress(raw);
    if (!validation.valid) {
        Json::Value err;
        err["error"] = validation.error;
        auto resp = drogon::HttpResponse::newHttpJsonResponse(err);
        resp->setStatusCode(static_cast<drogon::HttpStatusCode>(validation.status_code));
        callback(resp);
        return;
    }
    raw = InputValidator::sanitize(raw);

    std::string cleaned = g_preprocessor.process(raw);

    // FIX 3: language hint passed to normalize() → libpostal expand_address
    NormalizedAddress result = g_parser.normalize(cleaned, language, country);
    result.latency_ms = timer.elapsedMs();

    callback(drogon::HttpResponse::newHttpJsonResponse(result.toJson()));
}

} // namespace addr
