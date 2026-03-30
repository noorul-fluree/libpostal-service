#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <chrono>
#include <json/json.h>

namespace addr {

// =============================================================================
//  AddressComponent — a single parsed field
// =============================================================================
struct AddressComponent {
    std::string label;   // e.g. "house_number", "road", "city", "state", "postcode"
    std::string value;   // e.g. "123", "mg road", "bengaluru"
};

// =============================================================================
//  ParsedAddress — the result of parsing one address
// =============================================================================
struct ParsedAddress {
    std::string house_number;
    std::string road;
    std::string suburb;
    std::string city;
    std::string city_district;
    std::string state;
    std::string state_district;
    std::string postcode;
    std::string country;
    std::string unit;
    std::string level;
    std::string staircase;
    std::string entrance;
    std::string po_box;
    std::string raw_input;

    // Processing metadata
    double confidence = 0.0;
    std::vector<int> pipeline_phases;
    double latency_ms = 0.0;
    bool from_cache = false;
    std::string error;

    Json::Value toJson() const {
        Json::Value j;
        if (!house_number.empty()) j["house_number"] = house_number;
        if (!road.empty())         j["road"] = road;
        if (!suburb.empty())       j["suburb"] = suburb;
        if (!city.empty())         j["city"] = city;
        if (!city_district.empty()) j["city_district"] = city_district;
        if (!state.empty())        j["state"] = state;
        if (!state_district.empty()) j["state_district"] = state_district;
        if (!postcode.empty())     j["postcode"] = postcode;
        if (!country.empty())      j["country"] = country;
        if (!unit.empty())         j["unit"] = unit;
        if (!level.empty())        j["level"] = level;
        if (!po_box.empty())       j["po_box"] = po_box;

        j["confidence"] = confidence;
        j["latency_ms"] = latency_ms;
        j["from_cache"] = from_cache;

        Json::Value phases(Json::arrayValue);
        for (int p : pipeline_phases) phases.append(p);
        j["pipeline_phases"] = phases;

        if (!error.empty()) j["error"] = error;
        return j;
    }
};

// =============================================================================
//  NormalizedAddress — a set of possible normalizations
// =============================================================================
struct NormalizedAddress {
    std::string raw_input;
    std::vector<std::string> normalizations;
    double latency_ms = 0.0;

    Json::Value toJson() const {
        Json::Value j;
        j["raw_input"] = raw_input;
        Json::Value norms(Json::arrayValue);
        for (const auto& n : normalizations) norms.append(n);
        j["normalizations"] = norms;
        j["latency_ms"] = latency_ms;
        return j;
    }
};

// =============================================================================
//  BatchRequest / BatchResponse
// =============================================================================
struct BatchRequest {
    std::vector<std::string> addresses;
    std::string default_country;  // optional hint
    std::string default_language; // optional hint

    static BatchRequest fromJson(const Json::Value& j) {
        BatchRequest req;
        if (j.isMember("addresses") && j["addresses"].isArray()) {
            for (const auto& a : j["addresses"]) {
                if (a.isString()) req.addresses.push_back(a.asString());
            }
        }
        if (j.isMember("country")) req.default_country = j["country"].asString();
        if (j.isMember("language")) req.default_language = j["language"].asString();
        return req;
    }
};

struct BatchResponse {
    std::vector<ParsedAddress> results;
    int total = 0;
    int succeeded = 0;
    int failed = 0;
    double total_latency_ms = 0.0;

    Json::Value toJson() const {
        Json::Value j;
        j["total"] = total;
        j["succeeded"] = succeeded;
        j["failed"] = failed;
        j["total_latency_ms"] = total_latency_ms;

        Json::Value arr(Json::arrayValue);
        for (const auto& r : results) arr.append(r.toJson());
        j["results"] = arr;
        return j;
    }
};

// =============================================================================
//  ServiceConfig — loaded from config.json
// =============================================================================
struct ServiceConfig {
    // Server
    int port = 8080;
    int threads = 0;  // 0 = auto-detect from CPU
    int max_connections = 100000;

    // libpostal
    std::string libpostal_data_dir = "/usr/share/libpostal";
    std::string geonames_data_dir;   // path to geonames data files
    std::string default_country;
    std::string default_language;

    // Cache
    bool cache_enabled = true;
    size_t cache_max_entries = 5000000;
    int cache_ttl_seconds = 86400;

    // Batch
    int batch_max_size = 2000;
    int batch_timeout_ms = 30000;

    // Rule engine
    bool rules_enabled = true;

    // LLM
    bool llm_enabled = false;
    std::string llm_model_path;
    double llm_confidence_threshold = 0.85;
    double llm_low_threshold = 0.70;
    int llm_max_concurrent = 4;
    int llm_timeout_ms = 5000;

    // Metrics
    bool metrics_enabled = true;
    int metrics_port = 9090;

    // Security
    bool auth_enabled = false;
    std::string auth_mode = "api_key";  // "api_key", "jwt", "both"
    std::string auth_api_key;
    std::string auth_keys_file;
    int max_address_length = 500;
    double cache_min_confidence = 0.5;

    // JWT Auth (calls your Node.js auth service)
    std::string jwt_auth_url = "http://localhost:3000/auth/jwt";
    int jwt_auth_timeout_ms = 3000;
    int jwt_cache_ttl_seconds = 300;  // cache valid tokens for 5 min
    int jwt_cache_max_entries = 10000;

    static ServiceConfig fromJson(const Json::Value& root) {
        ServiceConfig cfg;
        if (root.isMember("server")) {
            auto& s = root["server"];
            cfg.port = s.get("port", 8080).asInt();
            cfg.threads = s.get("threads", 0).asInt();
            cfg.max_connections = s.get("max_connections", 100000).asInt();
        }
        if (root.isMember("libpostal")) {
            auto& lp = root["libpostal"];
            cfg.libpostal_data_dir = lp.get("data_dir", "/usr/share/libpostal").asString();
            cfg.default_country = lp.get("default_country", "").asString();
            cfg.default_language = lp.get("default_language", "").asString();
        }
        if (root.isMember("geonames")) {
            cfg.geonames_data_dir = root["geonames"].get("data_dir", "").asString();
        }
        if (root.isMember("cache")) {
            auto& c = root["cache"];
            cfg.cache_enabled = c.get("enabled", true).asBool();
            cfg.cache_max_entries = c.get("max_entries", 5000000).asUInt64();
            cfg.cache_ttl_seconds = c.get("ttl_seconds", 86400).asInt();
        }
        if (root.isMember("batch")) {
            auto& b = root["batch"];
            cfg.batch_max_size = b.get("max_size", 2000).asInt();
            cfg.batch_timeout_ms = b.get("timeout_ms", 30000).asInt();
        }
        if (root.isMember("rules_engine")) {
            cfg.rules_enabled = root["rules_engine"].get("enabled", true).asBool();
        }
        if (root.isMember("llm")) {
            auto& l = root["llm"];
            cfg.llm_enabled = l.get("enabled", false).asBool();
            cfg.llm_model_path = l.get("model_path", "").asString();
            cfg.llm_confidence_threshold = l.get("confidence_threshold", 0.85).asDouble();
            cfg.llm_low_threshold = l.get("low_threshold", 0.70).asDouble();
            cfg.llm_max_concurrent = l.get("max_concurrent", 4).asInt();
            cfg.llm_timeout_ms = l.get("timeout_ms", 5000).asInt();
        }
        if (root.isMember("metrics")) {
            cfg.metrics_enabled = root["metrics"].get("enabled", true).asBool();
            cfg.metrics_port = root["metrics"].get("port", 9090).asInt();
        }
        if (root.isMember("security")) {
            auto& s = root["security"];
            cfg.auth_enabled = s.get("enabled", false).asBool();
            cfg.auth_mode = s.get("mode", "api_key").asString();
            cfg.auth_api_key = s.get("api_key", "").asString();
            cfg.auth_keys_file = s.get("keys_file", "").asString();
            cfg.max_address_length = s.get("max_address_length", 500).asInt();
            cfg.cache_min_confidence = s.get("cache_min_confidence", 0.5).asDouble();
        }
        if (root.isMember("jwt")) {
            auto& j = root["jwt"];
            cfg.jwt_auth_url = j.get("auth_url", "http://localhost:3000/auth/jwt").asString();
            cfg.jwt_auth_timeout_ms = j.get("timeout_ms", 3000).asInt();
            cfg.jwt_cache_ttl_seconds = j.get("cache_ttl_seconds", 300).asInt();
            cfg.jwt_cache_max_entries = j.get("cache_max_entries", 10000).asInt();
        }
        return cfg;
    }
};

// =============================================================================
//  Timer utility
// =============================================================================
class ScopedTimer {
public:
    ScopedTimer() : start_(std::chrono::high_resolution_clock::now()) {}

    double elapsedMs() const {
        auto now = std::chrono::high_resolution_clock::now();
        return std::chrono::duration<double, std::milli>(now - start_).count();
    }

private:
    std::chrono::high_resolution_clock::time_point start_;
};

} // namespace addr
