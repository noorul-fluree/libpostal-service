#pragma once

#include "models/AddressModels.h"
#include <string>
#include <atomic>
#include <mutex>

namespace addr {

// =============================================================================
//  LLMFallback — Phase 4 LLM improvement (llama.cpp / Mistral 7B)
//
//  Runtime kill-switch:
//    Set environment variable  DISABLE_LLM=true  to bypass Phase 4 entirely
//    at runtime without recompiling or changing config.json.
//
//  When DISABLE_LLM=true:
//    - isReady()   returns false
//    - improve()   returns false immediately (no inference, no latency)
//    - The pipeline falls through directly to "Return result" as if
//      Phase 4 never existed — pure Phases 1-2-3 C++ core path.
//
//  When DISABLE_LLM is unset or "false":
//    - Normal flow: LLM is loaded if llm_enabled=true in config and
//      a model path is provided. improve() runs inference as before.
//
//  This flag is checked once at initialize() time and cached as an atomic
//  so isReady() / improve() pay zero cost to check it at call time.
//
//  Typical use cases:
//    - Production deploy: DISABLE_LLM=true  → zero GPU/CPU overhead
//    - Staging/testing:   DISABLE_LLM=false → full pipeline enabled
//    - Emergency toggle:  restart pod with DISABLE_LLM=true to shed load
// =============================================================================

class LLMFallback {
public:
    LLMFallback();
    ~LLMFallback();

    // Initialize the LLM model. Respects DISABLE_LLM env var.
    // Returns false (and logs why) if disabled or model load fails.
    bool initialize(const std::string& model_path, int max_concurrent = 4);

    // Returns true only if: DISABLE_LLM is not set AND model loaded OK.
    bool isReady() const { return ready_.load(std::memory_order_relaxed); }

    // Returns true if LLM was explicitly disabled via DISABLE_LLM flag.
    bool isDisabledByFlag() const { return disabled_by_flag_.load(std::memory_order_relaxed); }

    // Attempt to improve a low-confidence address. No-op if !isReady().
    bool improve(ParsedAddress& parsed, int timeout_ms = 5000);

    void shutdown();

private:
    // ready_ = initialized AND not disabled by flag
    std::atomic<bool> ready_{false};
    // disabled_by_flag_ = DISABLE_LLM=true was set at startup
    std::atomic<bool> disabled_by_flag_{false};

    std::string buildPrompt(const ParsedAddress& parsed) const;
    bool parseLLMResponse(const std::string& response, ParsedAddress& parsed) const;

#ifdef ENABLE_LLM
    void* model_ = nullptr;
    void* ctx_   = nullptr;
    std::mutex inference_mutex_;
#endif
};

} // namespace addr
