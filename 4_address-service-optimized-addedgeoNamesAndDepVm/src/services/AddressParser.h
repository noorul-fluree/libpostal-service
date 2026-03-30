#pragma once

#include "models/AddressModels.h"
#include <string>
#include <mutex>
#include <atomic>
#include <array>
#include <string_view>
#include <unordered_map>

namespace addr {

class AddressParser {
public:
    bool initialize(const std::string& data_dir);
    void shutdown();

    ParsedAddress     parse(const std::string& address,
                            const std::string& language = "",
                            const std::string& country  = "") const;

    NormalizedAddress normalize(const std::string& address,
                                const std::string& language = "",
                                const std::string& country  = "") const;

    struct DedupeResult {
        std::vector<int>    canonical_index;
        std::vector<size_t> unique_indices;
    };
    DedupeResult deduplicateBatch(const std::vector<std::string>& addresses) const;

    bool isReady() const { return initialized_.load(std::memory_order_relaxed); }

    bool initLanguageClassifier();
    void teardownLanguageClassifier();

private:
    std::atomic<bool> initialized_{false};
    std::atomic<bool> classifier_ready_{false};
    static std::once_flag init_flag_;

    // ← ADDED: store data_dir so initLanguageClassifier() can use it
    std::string data_dir_;

    static constexpr int MUTEX_SLOTS = 16;
    mutable std::array<std::mutex, MUTEX_SLOTS> parse_mutexes_;
    mutable std::array<std::mutex, MUTEX_SLOTS> expand_mutexes_;

    std::mutex& parseMutex(const std::string& addr) const noexcept {
        return parse_mutexes_[
            std::hash<std::string>{}(addr) & (MUTEX_SLOTS - 1)
        ];
    }
    std::mutex& expandMutex(const std::string& addr) const noexcept {
        return expand_mutexes_[
            std::hash<std::string>{}(addr) & (MUTEX_SLOTS - 1)
        ];
    }

    using FieldPtr = std::string ParsedAddress::*;
    static const std::unordered_map<std::string_view, FieldPtr>& labelMap();
};

} // namespace addr