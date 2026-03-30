#include "services/LLMFallback.h"
#include <iostream>
#include <sstream>
#include <cstdlib>
#include <json/json.h>

#ifdef ENABLE_LLM
#include <llama.h>
#endif

namespace addr {

// =============================================================================
//  Helper: read DISABLE_LLM environment variable once
//  Returns true if the LLM should be disabled at runtime.
//
//  Accepted values for DISABLE_LLM (case-insensitive):
//    "true", "1", "yes", "on"   → disabled
//    anything else / not set    → enabled
// =============================================================================
static bool readDisableLLMFlag() {
    const char* env = std::getenv("DISABLE_LLM");
    if (!env) return false;
    std::string val(env);
    // lowercase comparison
    for (char& c : val) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return (val == "true" || val == "1" || val == "yes" || val == "on");
}

LLMFallback::LLMFallback()  {}
LLMFallback::~LLMFallback() { shutdown(); }

// =============================================================================
//  initialize
//
//  Step 1: check DISABLE_LLM flag — if set, log and return false immediately.
//          No model file is touched, no memory allocated.
//  Step 2: proceed with normal llama.cpp model load.
// =============================================================================
bool LLMFallback::initialize(const std::string& model_path, int max_concurrent) {
    // -------------------------------------------------------------------------
    //  KILL-SWITCH CHECK — evaluated first, before any model I/O
    // -------------------------------------------------------------------------
    if (readDisableLLMFlag()) {
        disabled_by_flag_.store(true,  std::memory_order_relaxed);
        ready_.store(false,            std::memory_order_relaxed);
        std::cout << "[LLMFallback] DISABLED by DISABLE_LLM environment variable.\n"
                  << "[LLMFallback] Pipeline will run Phases 1-2-3 only (C++ core path).\n"
                  << "[LLMFallback] To re-enable: unset DISABLE_LLM and restart.\n";
        return false;
    }

#ifdef ENABLE_LLM
    std::cout << "[LLMFallback] Loading model: " << model_path << std::endl;

    llama_backend_init();

    auto model_params      = llama_model_default_params();
    model_params.n_gpu_layers = 0; // CPU-only; set > 0 for GPU offload

    model_ = llama_load_model_from_file(model_path.c_str(), model_params);
    if (!model_) {
        std::cerr << "[LLMFallback] ERROR: failed to load model from " << model_path << "\n";
        return false;
    }

    auto ctx_params         = llama_context_default_params();
    ctx_params.n_ctx        = 2048;
    ctx_params.n_threads    = max_concurrent;

    ctx_ = llama_new_context_with_model(static_cast<llama_model*>(model_), ctx_params);
    if (!ctx_) {
        std::cerr << "[LLMFallback] ERROR: failed to create inference context\n";
        llama_free_model(static_cast<llama_model*>(model_));
        model_ = nullptr;
        return false;
    }

    ready_.store(true, std::memory_order_relaxed);
    std::cout << "[LLMFallback] Model loaded — Phase 4 active.\n";
    return true;

#else
    std::cout << "[LLMFallback] LLM support not compiled in (build with -DENABLE_LLM=ON).\n"
              << "[LLMFallback] Phase 4 will be skipped.\n";
    return false;
#endif
}

void LLMFallback::shutdown() {
#ifdef ENABLE_LLM
    if (ready_.load(std::memory_order_relaxed)) {
        if (ctx_) {
            llama_free(static_cast<llama_context*>(ctx_));
            ctx_ = nullptr;
        }
        if (model_) {
            llama_free_model(static_cast<llama_model*>(model_));
            model_ = nullptr;
        }
        llama_backend_free();
        ready_.store(false, std::memory_order_relaxed);
        std::cout << "[LLMFallback] Model unloaded.\n";
    }
#endif
}

// =============================================================================
//  improve — Phase 4 entry point
//
//  Guard: returns false immediately if:
//    - DISABLE_LLM flag was set (disabled_by_flag_ == true)
//    - Model not loaded         (ready_ == false)
//  Both checks are atomic reads — zero overhead on the fast path.
// =============================================================================
bool LLMFallback::improve(ParsedAddress& parsed, int timeout_ms) {
    // Fast-path guard — no inference if disabled or not ready
    if (!ready_.load(std::memory_order_relaxed)) return false;

#ifdef ENABLE_LLM
    std::lock_guard<std::mutex> lock(inference_mutex_);

    std::string prompt = buildPrompt(parsed);
    auto* model = static_cast<llama_model*>(model_);
    auto* ctx   = static_cast<llama_context*>(ctx_);

    // Tokenize prompt
    std::vector<llama_token> tokens(prompt.size() + 128);
    int n_tokens = llama_tokenize(model, prompt.c_str(), prompt.size(),
                                   tokens.data(), tokens.size(), true, false);
    if (n_tokens < 0) return false;
    tokens.resize(n_tokens);

    // Clear KV cache and evaluate prompt
    llama_kv_cache_clear(ctx);

    llama_batch batch = llama_batch_init(n_tokens, 0, 1);
    for (int i = 0; i < n_tokens; ++i)
        llama_batch_add(batch, tokens[i], i, {0}, false);
    batch.logits[batch.n_tokens - 1] = true;

    if (llama_decode(ctx, batch) != 0) {
        llama_batch_free(batch);
        return false;
    }

    // Generate response (greedy, max 512 tokens)
    std::string response;
    for (int i = 0; i < 512; ++i) {
        auto* logits    = llama_get_logits_ith(ctx, batch.n_tokens - 1);
        llama_token tok = llama_sample_token_greedy(ctx, logits, llama_n_vocab(model));

        if (llama_token_is_eog(model, tok)) break;

        char buf[64];
        int len = llama_token_to_piece(model, tok, buf, sizeof(buf), 0, false);
        if (len > 0) response.append(buf, len);

        llama_batch_clear(batch);
        llama_batch_add(batch, tok, n_tokens + i, {0}, true);
        if (llama_decode(ctx, batch) != 0) break;
    }
    llama_batch_free(batch);

    return parseLLMResponse(response, parsed);
#else
    return false;
#endif
}

// =============================================================================
//  buildPrompt — unchanged from original
// =============================================================================
std::string LLMFallback::buildPrompt(const ParsedAddress& parsed) const {
    std::ostringstream oss;
    oss << "Parse the following address into structured components. "
        << "Return ONLY a JSON object with these fields: "
        << "house_number, road, suburb, city, state, postcode, country. "
        << "Leave empty string for missing fields.\n\n"
        << "Address: " << parsed.raw_input << "\n\n";

    if (!parsed.city.empty() || !parsed.state.empty() || !parsed.postcode.empty()) {
        oss << "Partial parse (may contain errors):\n";
        if (!parsed.house_number.empty()) oss << "  house_number: " << parsed.house_number << "\n";
        if (!parsed.road.empty())         oss << "  road: "         << parsed.road         << "\n";
        if (!parsed.city.empty())         oss << "  city: "         << parsed.city         << "\n";
        if (!parsed.state.empty())        oss << "  state: "        << parsed.state        << "\n";
        if (!parsed.postcode.empty())     oss << "  postcode: "     << parsed.postcode     << "\n";
        oss << "\nPlease correct any errors and fill in missing fields.\n\n";
    }
    oss << "JSON:";
    return oss.str();
}

// =============================================================================
//  parseLLMResponse — unchanged from original
// =============================================================================
bool LLMFallback::parseLLMResponse(const std::string& response, ParsedAddress& parsed) const {
    size_t start = response.find('{');
    size_t end   = response.rfind('}');
    if (start == std::string::npos || end == std::string::npos || end <= start)
        return false;

    std::string json_str = response.substr(start, end - start + 1);

    Json::Value root;
    Json::CharReaderBuilder builder;
    std::string errors;
    std::istringstream stream(json_str);
    if (!Json::parseFromStream(builder, stream, &root, &errors))
        return false;

    bool improved = false;
    auto update = [&](std::string& field, const char* key) {
        if (root.isMember(key) && root[key].isString()) {
            std::string val = root[key].asString();
            if (!val.empty() && field != val) {
                field    = val;
                improved = true;
            }
        }
    };

    update(parsed.house_number, "house_number");
    update(parsed.road,         "road");
    update(parsed.suburb,       "suburb");
    update(parsed.city,         "city");
    update(parsed.state,        "state");
    update(parsed.postcode,     "postcode");
    update(parsed.country,      "country");

    return improved;
}

} // namespace addr
