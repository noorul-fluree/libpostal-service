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
#include <iostream>

extern addr::AddressParser     g_parser;
extern addr::PreProcessor      g_preprocessor;
extern addr::ConfidenceScorer  g_scorer;
extern addr::RuleEngine        g_rules;
extern addr::CacheManager*     g_cache;
extern addr::LLMFallback       g_llm;
extern addr::ServiceConfig     g_config;

namespace addr {

void ParseController::parse(const drogon::HttpRequestPtr& req,
                             std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
    ScopedTimer timer;
    auto& metrics = MetricsCollector::instance();

    auto json = req->getJsonObject();
    if (!json || !json->isMember("address")) {
        std::cout << "[ParseController] ✘ POST /api/v1/parse | error=Missing 'address' field\n";
        auto resp = drogon::HttpResponse::newHttpJsonResponse(
            Json::Value("Missing 'address' field in request body"));
        resp->setStatusCode(drogon::k400BadRequest);
        callback(resp); return;
    }

    std::string raw_address = (*json)["address"].asString();

    // Truncate for log display (max 60 chars)
    std::string addr_preview = raw_address.size() > 60
                               ? raw_address.substr(0, 60) + "..." : raw_address;

    std::string language = json->isMember("language") ? (*json)["language"].asString()
                                                       : g_config.default_language;
    std::string country  = json->isMember("country")  ? (*json)["country"].asString()
                                                       : g_config.default_country;

    auto validation = InputValidator::validateAddress(raw_address);
    if (!validation.valid) {
        std::cout << "[ParseController] ✘ POST /api/v1/parse"
                  << " | addr=\"" << addr_preview << "\""
                  << " | error=" << validation.error << "\n";
        Json::Value err;
        err["error"]        = validation.error;
        err["input_length"] = static_cast<int>(raw_address.size());
        auto resp = drogon::HttpResponse::newHttpJsonResponse(err);
        resp->setStatusCode(static_cast<drogon::HttpStatusCode>(validation.status_code));
        callback(resp); return;
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
            std::cout << "[ParseController] ✔ POST /api/v1/parse"
                      << " | addr=\"" << addr_preview << "\""
                      << " | cache_hit=true"
                      << " | confidence=" << cached->confidence
                      << " | latency=" << cached->latency_ms << "ms\n";
            callback(drogon::HttpResponse::newHttpJsonResponse(cached->toJson()));
            return;
        }
        metrics.recordCacheMiss();
    }

    // Phase 1
    std::string cleaned = g_preprocessor.process(raw_address);
    metrics.recordPhase(1);

    // Phase 2
    ParsedAddress result = g_parser.parse(cleaned, language, country);
    result.raw_input = raw_address;
    result.pipeline_phases.push_back(1);
    result.pipeline_phases.push_back(2);
    metrics.recordPhase(2);

    if (!result.error.empty()) {
        result.latency_ms = timer.elapsedMs();
        metrics.recordParse(result.latency_ms, false);
        std::cout << "[ParseController] ✘ POST /api/v1/parse"
                  << " | addr=\"" << addr_preview << "\""
                  << " | error=" << result.error
                  << " | latency=" << result.latency_ms << "ms\n";
        auto resp = drogon::HttpResponse::newHttpJsonResponse(result.toJson());
        resp->setStatusCode(drogon::k500InternalServerError);
        callback(resp); return;
    }

    result.confidence = g_scorer.score(result);
    metrics.recordConfidence(result.confidence);

    // Phase 3
    if (g_config.rules_enabled && result.confidence < g_config.llm_confidence_threshold) {
        bool improved = g_rules.apply(result);
        if (improved) {
            result.pipeline_phases.push_back(3);
            result.confidence = g_scorer.score(result);
            metrics.recordPhase(3);
        }
    }

    // Phase 4
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
        result.confidence >= g_config.cache_min_confidence)
        g_cache->put(raw_address, result);

    metrics.recordParse(result.latency_ms, result.error.empty());

    std::cout << "[ParseController] ✔ POST /api/v1/parse"
              << " | addr=\"" << addr_preview << "\""
              << " | cache_hit=false"
              << " | confidence=" << result.confidence
              << " | latency=" << result.latency_ms << "ms\n";

    callback(drogon::HttpResponse::newHttpJsonResponse(result.toJson()));
}

void ParseController::normalize(const drogon::HttpRequestPtr& req,
                                 std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
    ScopedTimer timer;

    auto json = req->getJsonObject();
    if (!json || !json->isMember("address")) {
        std::cout << "[ParseController] ✘ POST /api/v1/normalize | error=Missing 'address' field\n";
        auto resp = drogon::HttpResponse::newHttpJsonResponse(Json::Value("Missing 'address' field"));
        resp->setStatusCode(drogon::k400BadRequest);
        callback(resp); return;
    }

    std::string raw = (*json)["address"].asString();
    std::string addr_preview = raw.size() > 60 ? raw.substr(0, 60) + "..." : raw;

    std::string language = json->isMember("language") ? (*json)["language"].asString()
                                                       : g_config.default_language;
    std::string country  = json->isMember("country")  ? (*json)["country"].asString()
                                                       : g_config.default_country;

    auto validation = InputValidator::validateAddress(raw);
    if (!validation.valid) {
        std::cout << "[ParseController] ✘ POST /api/v1/normalize"
                  << " | addr=\"" << addr_preview << "\""
                  << " | error=" << validation.error << "\n";
        Json::Value err; err["error"] = validation.error;
        auto resp = drogon::HttpResponse::newHttpJsonResponse(err);
        resp->setStatusCode(static_cast<drogon::HttpStatusCode>(validation.status_code));
        callback(resp); return;
    }
    raw = InputValidator::sanitize(raw);

    std::string cleaned = g_preprocessor.process(raw);
    NormalizedAddress result = g_parser.normalize(cleaned, language, country);
    result.latency_ms = timer.elapsedMs();

    std::cout << "[ParseController] ✔ POST /api/v1/normalize"
              << " | addr=\"" << addr_preview << "\""
              << " | expansions=" << result.normalizations.size()
              << " | latency=" << result.latency_ms << "ms\n";

    callback(drogon::HttpResponse::newHttpJsonResponse(result.toJson()));
}

} // namespace addr