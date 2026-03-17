#pragma once

#include "models/AddressModels.h"
#include <unordered_map>
#include <string>

namespace addr {

class RuleEngine {
public:
    RuleEngine();

    // Apply business rules to improve a parsed address
    // Returns true if any corrections were made
    bool apply(ParsedAddress& parsed) const;

private:
    // Individual rule sets
    bool inferStateFromIndianPIN(ParsedAddress& p) const;
    bool inferCityFromIndianPIN(ParsedAddress& p) const;
    bool validateAndFixUSZip(ParsedAddress& p) const;
    bool normalizeStateName(ParsedAddress& p) const;
    bool inferCountry(ParsedAddress& p) const;
    bool fixCommonMisspellings(ParsedAddress& p) const;

    // Lookup tables
    std::unordered_map<std::string, std::string> pin_to_state_;       // first 2 digits -> state
    std::unordered_map<std::string, std::string> pin_to_city_;        // first 3 digits -> major city
    std::unordered_map<std::string, std::string> state_abbrev_to_full_;  // US state abbrevs
    std::unordered_map<std::string, std::string> indian_state_normalize_; // common variants
    std::unordered_map<std::string, std::string> common_misspellings_;

    void initIndianPINData();
    void initUSStateData();
    void initMisspellings();
};

} // namespace addr
