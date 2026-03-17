#pragma once

#include "models/AddressModels.h"
#include <string>
#include <mutex>
#include <atomic>

namespace addr {

class AddressParser {
public:
    // Initialize libpostal (must be called once at startup)
    bool initialize(const std::string& data_dir);

    // Shutdown libpostal (call at exit)
    void shutdown();

    // Parse a single address into components
    ParsedAddress parse(const std::string& address) const;

    // Normalize an address (returns multiple possible normalizations)
    NormalizedAddress normalize(const std::string& address) const;

    // Check if initialized
    bool isReady() const { return initialized_.load(); }

private:
    std::atomic<bool> initialized_{false};
    static std::once_flag init_flag_;
};

} // namespace addr
