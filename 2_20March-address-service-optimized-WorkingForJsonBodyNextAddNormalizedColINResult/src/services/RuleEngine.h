#pragma once

#include "models/AddressModels.h"
#include <unordered_map>
#include <string>
#include <string_view>

namespace addr {

// =============================================================================
//  RuleEngine — optimized rule application
//
//  Key optimizations vs original:
//   1. Early-exit fast path: skip all PIN rules when postcode is wrong length
//   2. std::regex removed from hot path — US ZIP validated with hand-rolled check
//   3. string_view used for comparisons to avoid copies
//   4. PIN prefix extracted as uint16_t (integer compare, no substr alloc)
//   5. Cross-field scoring uses char comparison, not regex
// =============================================================================

class RuleEngine {
public:
    RuleEngine();

    bool apply(ParsedAddress& parsed) const;

private:
    bool inferStateFromIndianPIN(ParsedAddress& p) const;
    bool inferCityFromIndianPIN(ParsedAddress& p) const;
    bool validateAndFixUSZip(ParsedAddress& p) const;
    bool normalizeStateName(ParsedAddress& p) const;
    bool inferCountry(ParsedAddress& p) const;
    bool fixCommonMisspellings(ParsedAddress& p) const;

    // Fast postcode type detection (no regex)
    static bool isIndianPIN(std::string_view pc) noexcept;
    static bool isUSZip(std::string_view pc) noexcept;

    // Lookup tables
    std::unordered_map<std::string, std::string> pin_to_state_;
    std::unordered_map<std::string, std::string> pin_to_city_;
    std::unordered_map<std::string, std::string> state_abbrev_to_full_;
    std::unordered_map<std::string, std::string> indian_state_normalize_;
    std::unordered_map<std::string, std::string> common_misspellings_;

    void initIndianPINData();
    void initUSStateData();
    void initMisspellings();
};

} // namespace addr
