#pragma once

#include <string>
#include <json/json.h>

namespace addr {

// =============================================================================
//  InputValidator — security boundary for all incoming requests
//
//  Enforces:
//    - Max address length (prevents CPU DoS via CRF model)
//    - Max batch size (prevents memory exhaustion)
//    - UTF-8 validity (prevents encoding attacks)
//    - Null byte rejection (prevents C string truncation attacks)
//    - Control character stripping (prevents log injection)
// =============================================================================
struct ValidationResult {
    bool valid = true;
    std::string error;
    int status_code = 200;
};

class InputValidator {
public:
    // Configuration
    static constexpr size_t MAX_ADDRESS_LENGTH = 500;    // chars
    static constexpr size_t MIN_ADDRESS_LENGTH = 3;      // chars
    static constexpr int MAX_BATCH_SIZE = 5000;
    static constexpr size_t MAX_BODY_SIZE = 10 * 1024 * 1024; // 10 MB
    // static constexpr size_t MAX_BODY_SIZE = 50 * 1024 * 1024; // 50 MB

    // Validate a single address string
    static ValidationResult validateAddress(const std::string& address) {
        ValidationResult result;

        // Empty check
        if (address.empty()) {
            result.valid = false;
            result.error = "Empty address";
            result.status_code = 400;
            return result;
        }

        // Min length
        if (address.size() < MIN_ADDRESS_LENGTH) {
            result.valid = false;
            result.error = "Address too short (minimum " +
                          std::to_string(MIN_ADDRESS_LENGTH) + " characters)";
            result.status_code = 400;
            return result;
        }

        // Max length (critical: prevents CPU DoS in libpostal CRF)
        if (address.size() > MAX_ADDRESS_LENGTH) {
            result.valid = false;
            result.error = "Address too long (" + std::to_string(address.size()) +
                          " chars, maximum " + std::to_string(MAX_ADDRESS_LENGTH) + ")";
            result.status_code = 400;
            return result;
        }

        // Null byte check (C string truncation attack)
        if (address.find('\0') != std::string::npos) {
            result.valid = false;
            result.error = "Address contains null bytes";
            result.status_code = 400;
            return result;
        }

        // Control character check (log injection prevention)
        for (unsigned char c : address) {
            if (c < 0x20 && c != '\t' && c != '\n' && c != '\r') {
                result.valid = false;
                result.error = "Address contains invalid control characters";
                result.status_code = 400;
                return result;
            }
        }

        // Basic UTF-8 validation
        if (!isValidUTF8(address)) {
            result.valid = false;
            result.error = "Address contains invalid UTF-8 encoding";
            result.status_code = 400;
            return result;
        }

        return result;
    }

    // Validate a batch request
    static ValidationResult validateBatch(const Json::Value& json, int max_batch_size) {
        ValidationResult result;

        if (!json.isMember("addresses") || !json["addresses"].isArray()) {
            result.valid = false;
            result.error = "Missing or invalid 'addresses' array";
            result.status_code = 400;
            return result;
        }

        int size = static_cast<int>(json["addresses"].size());

        if (size == 0) {
            result.valid = false;
            result.error = "Empty addresses array";
            result.status_code = 400;
            return result;
        }

        if (size > max_batch_size) {
            result.valid = false;
            result.error = "Batch size " + std::to_string(size) +
                          " exceeds maximum of " + std::to_string(max_batch_size);
            result.status_code = 400;
            return result;
        }

        return result;
    }

    // Sanitize an address (strip dangerous chars, keep the rest)
    static std::string sanitize(const std::string& input) {
        std::string result;
        result.reserve(input.size());
        for (unsigned char c : input) {
            // Strip control chars except tab/newline (converted to space)
            if (c < 0x20) {
                if (c == '\t' || c == '\n' || c == '\r') {
                    result += ' ';
                }
                // Other control chars silently dropped
            } else if (c == 0x7F) {
                // DEL character — drop
            } else {
                result += static_cast<char>(c);
            }
        }
        return result;
    }

private:
    // Basic UTF-8 validation (checks byte sequence structure)
    static bool isValidUTF8(const std::string& s) {
        size_t i = 0;
        while (i < s.size()) {
            unsigned char c = static_cast<unsigned char>(s[i]);
            int bytes = 0;

            if (c <= 0x7F) {
                bytes = 1;
            } else if ((c & 0xE0) == 0xC0) {
                bytes = 2;
            } else if ((c & 0xF0) == 0xE0) {
                bytes = 3;
            } else if ((c & 0xF8) == 0xF0) {
                bytes = 4;
            } else {
                return false; // Invalid leading byte
            }

            if (i + bytes > s.size()) return false; // Truncated sequence

            // Verify continuation bytes
            for (int j = 1; j < bytes; ++j) {
                if ((static_cast<unsigned char>(s[i + j]) & 0xC0) != 0x80) {
                    return false;
                }
            }

            // Overlong encoding check
            if (bytes == 2 && c < 0xC2) return false;
            if (bytes == 3) {
                unsigned char c1 = static_cast<unsigned char>(s[i + 1]);
                if (c == 0xE0 && c1 < 0xA0) return false;
            }
            if (bytes == 4) {
                unsigned char c1 = static_cast<unsigned char>(s[i + 1]);
                if (c == 0xF0 && c1 < 0x90) return false;
                if (c == 0xF4 && c1 > 0x8F) return false;
                if (c > 0xF4) return false;
            }

            i += bytes;
        }
        return true;
    }
};

} // namespace addr
