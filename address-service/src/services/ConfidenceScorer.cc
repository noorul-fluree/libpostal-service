#include "services/ConfidenceScorer.h"
#include <regex>
#include <sstream>
#include <algorithm>

namespace addr {

double ConfidenceScorer::score(const ParsedAddress& parsed) const {
    return scoreDetailed(parsed).total;
}

ConfidenceScorer::ScoreBreakdown ConfidenceScorer::scoreDetailed(const ParsedAddress& parsed) const {
    ScoreBreakdown bd;
    bd.completeness = scoreCompleteness(parsed);
    bd.postcode_validity = scorePostcodeValidity(parsed);
    bd.cross_field = scoreCrossField(parsed);
    bd.token_coverage = scoreTokenCoverage(parsed);

    // Weighted average
    bd.total = bd.completeness * 0.35 +
               bd.postcode_validity * 0.25 +
               bd.cross_field * 0.20 +
               bd.token_coverage * 0.20;

    return bd;
}

double ConfidenceScorer::scoreCompleteness(const ParsedAddress& p) const {
    int present = 0;
    int total_fields = 5; // house_number, road, city, state, postcode

    if (!p.house_number.empty()) ++present;
    if (!p.road.empty())         ++present;
    if (!p.city.empty())         ++present;
    if (!p.state.empty())        ++present;
    if (!p.postcode.empty())     ++present;

    // Bonus for having suburb or country
    double base = static_cast<double>(present) / total_fields;
    if (!p.suburb.empty())  base = std::min(1.0, base + 0.05);
    if (!p.country.empty()) base = std::min(1.0, base + 0.05);

    return base;
}

double ConfidenceScorer::scorePostcodeValidity(const ParsedAddress& p) const {
    if (p.postcode.empty()) return 0.3; // No postcode is not great but not fatal

    const std::string& pc = p.postcode;

    // India: 6-digit PIN
    static const std::regex india_pin("^[1-9][0-9]{5}$");
    if (std::regex_match(pc, india_pin)) return 1.0;

    // US: 5-digit or 5+4 ZIP
    static const std::regex us_zip("^[0-9]{5}(-[0-9]{4})?$");
    if (std::regex_match(pc, us_zip)) return 1.0;

    // UK: various formats
    static const std::regex uk_post("^[A-Z]{1,2}[0-9][A-Z0-9]?\\s?[0-9][A-Z]{2}$",
                                     std::regex_constants::icase);
    if (std::regex_match(pc, uk_post)) return 1.0;

    // Canada: A1A 1A1
    static const std::regex ca_post("^[A-Z][0-9][A-Z]\\s?[0-9][A-Z][0-9]$",
                                     std::regex_constants::icase);
    if (std::regex_match(pc, ca_post)) return 1.0;

    // Australia: 4-digit
    static const std::regex au_post("^[0-9]{4}$");
    if (std::regex_match(pc, au_post)) return 0.9;

    // Generic: has digits, reasonable length
    if (pc.size() >= 3 && pc.size() <= 10) return 0.6;

    return 0.3;
}

double ConfidenceScorer::scoreCrossField(const ParsedAddress& p) const {
    double score = 0.7; // Default baseline

    // India: validate PIN prefix against state
    if (!p.postcode.empty() && p.postcode.size() == 6 && !p.state.empty()) {
        char first_digit = p.postcode[0];
        std::string state_lower = p.state;
        std::transform(state_lower.begin(), state_lower.end(), state_lower.begin(), ::tolower);

        // Indian PIN first digit roughly maps to regions
        bool matches = false;
        if (first_digit == '1' && (state_lower.find("delhi") != std::string::npos ||
                                    state_lower.find("haryana") != std::string::npos ||
                                    state_lower.find("punjab") != std::string::npos ||
                                    state_lower.find("himachal") != std::string::npos))
            matches = true;
        else if (first_digit == '2' && (state_lower.find("uttar pradesh") != std::string::npos ||
                                         state_lower.find("uttarakhand") != std::string::npos))
            matches = true;
        else if (first_digit == '3' && (state_lower.find("rajasthan") != std::string::npos ||
                                         state_lower.find("gujarat") != std::string::npos))
            matches = true;
        else if (first_digit == '4' && (state_lower.find("maharashtra") != std::string::npos ||
                                         state_lower.find("goa") != std::string::npos ||
                                         state_lower.find("madhya") != std::string::npos))
            matches = true;
        else if (first_digit == '5' && (state_lower.find("andhra") != std::string::npos ||
                                         state_lower.find("telangana") != std::string::npos ||
                                         state_lower.find("karnataka") != std::string::npos))
            matches = true;
        else if (first_digit == '6' && (state_lower.find("tamil") != std::string::npos ||
                                         state_lower.find("kerala") != std::string::npos))
            matches = true;
        else if (first_digit == '7' && (state_lower.find("west bengal") != std::string::npos ||
                                         state_lower.find("odisha") != std::string::npos ||
                                         state_lower.find("assam") != std::string::npos ||
                                         state_lower.find("northeast") != std::string::npos))
            matches = true;
        else if (first_digit == '8' && (state_lower.find("bihar") != std::string::npos ||
                                         state_lower.find("jharkhand") != std::string::npos))
            matches = true;

        if (matches) {
            score = 1.0;
        } else {
            score = 0.5; // Mismatch, but not definitely wrong
        }
    }

    // US: check if state abbreviation is valid
    if (!p.state.empty() && !p.postcode.empty()) {
        static const std::regex us_zip("^[0-9]{5}");
        if (std::regex_search(p.postcode, us_zip) && p.state.size() == 2) {
            score = std::max(score, 0.8); // Has ZIP + 2-letter state
        }
    }

    return score;
}

double ConfidenceScorer::scoreTokenCoverage(const ParsedAddress& p) const {
    if (p.raw_input.empty()) return 0.0;

    // Count input tokens
    std::istringstream iss(p.raw_input);
    std::string token;
    int total_tokens = 0;
    while (iss >> token) ++total_tokens;
    if (total_tokens == 0) return 0.0;

    // Count assigned tokens (rough estimate from populated fields)
    int assigned_chars = 0;
    assigned_chars += p.house_number.size();
    assigned_chars += p.road.size();
    assigned_chars += p.suburb.size();
    assigned_chars += p.city.size();
    assigned_chars += p.state.size();
    assigned_chars += p.postcode.size();
    assigned_chars += p.country.size();
    assigned_chars += p.unit.size();

    // Compare total parsed chars vs input chars (minus punctuation)
    int input_alpha_chars = 0;
    for (char c : p.raw_input) {
        if (std::isalnum(static_cast<unsigned char>(c))) ++input_alpha_chars;
    }

    if (input_alpha_chars == 0) return 0.5;
    double ratio = static_cast<double>(assigned_chars) / input_alpha_chars;
    return std::min(1.0, ratio);
}

} // namespace addr
