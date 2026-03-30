#include "services/RuleEngine.h"
#include <algorithm>
#include <cctype>

namespace addr {

// =============================================================================
//  Fast postcode detectors — replaces std::regex in hot path
// =============================================================================

bool RuleEngine::isIndianPIN(std::string_view pc) noexcept {
    if (pc.size() != 6) return false;
    if (pc[0] < '1' || pc[0] > '8') return false;
    for (char c : pc)
        if (c < '0' || c > '9') return false;
    return true;
}

bool RuleEngine::isUSZip(std::string_view pc) noexcept {
    // 5 digits, or 5 digits + '-' + 4 digits
    if (pc.size() != 5 && pc.size() != 10) return false;
    for (int i = 0; i < 5; ++i)
        if (pc[i] < '0' || pc[i] > '9') return false;
    if (pc.size() == 10) {
        if (pc[5] != '-') return false;
        for (int i = 6; i < 10; ++i)
            if (pc[i] < '0' || pc[i] > '9') return false;
    }
    return true;
}

// =============================================================================
//  apply — ordered rule application with early exits
// =============================================================================

RuleEngine::RuleEngine() {
    initIndianPINData();
    initUSStateData();
    initMisspellings();
}

bool RuleEngine::apply(ParsedAddress& parsed) const {
    bool modified = false;
    modified |= fixCommonMisspellings(parsed);
    modified |= normalizeStateName(parsed);

    // PIN-based rules only when postcode has right length — avoids 6 map lookups
    if (!parsed.postcode.empty()) {
        if (isIndianPIN(parsed.postcode)) {
            modified |= inferStateFromIndianPIN(parsed);
            modified |= inferCityFromIndianPIN(parsed);
            modified |= inferCountry(parsed); // will set "india" fast
            return modified;
        }
        if (isUSZip(parsed.postcode)) {
            modified |= validateAndFixUSZip(parsed);
            modified |= inferCountry(parsed); // will set "united states" fast
            return modified;
        }
    }

    modified |= inferCountry(parsed);
    return modified;
}

bool RuleEngine::inferStateFromIndianPIN(ParsedAddress& p) const {
    if (!p.state.empty()) return false;
    // Extract 2-digit prefix as string_view — no substr alloc
    std::string_view prefix(p.postcode.data(), 2);
    auto it = pin_to_state_.find(std::string(prefix));
    if (it != pin_to_state_.end()) {
        p.state = it->second;
        return true;
    }
    return false;
}

bool RuleEngine::inferCityFromIndianPIN(ParsedAddress& p) const {
    if (!p.city.empty()) return false;
    std::string_view prefix(p.postcode.data(), 3);
    auto it = pin_to_city_.find(std::string(prefix));
    if (it != pin_to_city_.end()) {
        p.city = it->second;
        return true;
    }
    return false;
}

bool RuleEngine::validateAndFixUSZip(ParsedAddress& p) const {
    if (p.country.empty()) {
        // Check state is a known US 2-letter abbreviation
        if (p.state.size() == 2) {
            std::string st_upper = p.state;
            std::transform(st_upper.begin(), st_upper.end(), st_upper.begin(), ::toupper);
            if (state_abbrev_to_full_.count(st_upper)) {
                p.country = "united states";
                return true;
            }
        }
    }
    return false;
}

bool RuleEngine::normalizeStateName(ParsedAddress& p) const {
    if (p.state.empty()) return false;
    std::string lower = p.state;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    auto it = indian_state_normalize_.find(lower);
    if (it != indian_state_normalize_.end() && it->second != lower) {
        p.state = it->second;
        return true;
    }
    return false;
}

bool RuleEngine::inferCountry(ParsedAddress& p) const {
    if (!p.country.empty()) return false;
    if (isIndianPIN(p.postcode)) { p.country = "india";         return true; }
    if (isUSZip(p.postcode))     { p.country = "united states"; return true; }
    return false;
}

bool RuleEngine::fixCommonMisspellings(ParsedAddress& p) const {
    bool modified = false;
    auto fix = [&](std::string& field) {
        if (field.empty()) return;
        std::string lower = field;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        auto it = common_misspellings_.find(lower);
        if (it != common_misspellings_.end()) {
            field    = it->second;
            modified = true;
        }
    };
    fix(p.city);
    fix(p.state);
    return modified;
}

// =============================================================================
//  Data tables — unchanged from original
// =============================================================================

void RuleEngine::initIndianPINData() {
    pin_to_state_["11"] = "delhi";          pin_to_state_["12"] = "haryana";
    pin_to_state_["13"] = "punjab";         pin_to_state_["14"] = "punjab";
    pin_to_state_["15"] = "himachal pradesh"; pin_to_state_["16"] = "punjab";
    pin_to_state_["17"] = "himachal pradesh"; pin_to_state_["18"] = "jammu and kashmir";
    pin_to_state_["19"] = "jammu and kashmir";
    pin_to_state_["20"] = "uttar pradesh";  pin_to_state_["21"] = "uttar pradesh";
    pin_to_state_["22"] = "uttar pradesh";  pin_to_state_["23"] = "uttar pradesh";
    pin_to_state_["24"] = "uttar pradesh";  pin_to_state_["25"] = "uttar pradesh";
    pin_to_state_["26"] = "uttarakhand";    pin_to_state_["27"] = "uttar pradesh";
    pin_to_state_["28"] = "uttar pradesh";
    pin_to_state_["30"] = "rajasthan";      pin_to_state_["31"] = "rajasthan";
    pin_to_state_["32"] = "rajasthan";      pin_to_state_["33"] = "rajasthan";
    pin_to_state_["34"] = "rajasthan";
    pin_to_state_["36"] = "gujarat";        pin_to_state_["37"] = "gujarat";
    pin_to_state_["38"] = "gujarat";        pin_to_state_["39"] = "gujarat";
    pin_to_state_["40"] = "maharashtra";    pin_to_state_["41"] = "maharashtra";
    pin_to_state_["42"] = "maharashtra";    pin_to_state_["43"] = "maharashtra";
    pin_to_state_["44"] = "maharashtra";
    pin_to_state_["45"] = "madhya pradesh"; pin_to_state_["46"] = "madhya pradesh";
    pin_to_state_["47"] = "madhya pradesh"; pin_to_state_["48"] = "madhya pradesh";
    pin_to_state_["49"] = "chhattisgarh";
    pin_to_state_["50"] = "telangana";      pin_to_state_["51"] = "andhra pradesh";
    pin_to_state_["52"] = "andhra pradesh"; pin_to_state_["53"] = "andhra pradesh";
    pin_to_state_["56"] = "karnataka";      pin_to_state_["57"] = "karnataka";
    pin_to_state_["58"] = "karnataka";      pin_to_state_["59"] = "karnataka";
    pin_to_state_["60"] = "tamil nadu";     pin_to_state_["61"] = "tamil nadu";
    pin_to_state_["62"] = "tamil nadu";     pin_to_state_["63"] = "tamil nadu";
    pin_to_state_["64"] = "tamil nadu";
    pin_to_state_["67"] = "kerala";         pin_to_state_["68"] = "kerala";
    pin_to_state_["69"] = "kerala";
    pin_to_state_["70"] = "west bengal";    pin_to_state_["71"] = "west bengal";
    pin_to_state_["72"] = "west bengal";    pin_to_state_["73"] = "west bengal";
    pin_to_state_["74"] = "west bengal";
    pin_to_state_["75"] = "odisha";         pin_to_state_["76"] = "odisha";
    pin_to_state_["77"] = "assam";          pin_to_state_["78"] = "assam";
    pin_to_state_["79"] = "northeast";
    pin_to_state_["80"] = "bihar";          pin_to_state_["81"] = "bihar";
    pin_to_state_["82"] = "jharkhand";      pin_to_state_["83"] = "jharkhand";
    pin_to_state_["84"] = "bihar";          pin_to_state_["85"] = "jharkhand";

    pin_to_city_["110"] = "new delhi";   pin_to_city_["400"] = "mumbai";
    pin_to_city_["560"] = "bengaluru";   pin_to_city_["600"] = "chennai";
    pin_to_city_["700"] = "kolkata";     pin_to_city_["500"] = "hyderabad";
    pin_to_city_["380"] = "ahmedabad";   pin_to_city_["411"] = "pune";
    pin_to_city_["302"] = "jaipur";      pin_to_city_["226"] = "lucknow";
    pin_to_city_["682"] = "kochi";       pin_to_city_["440"] = "nagpur";
    pin_to_city_["800"] = "patna";       pin_to_city_["462"] = "bhopal";
    pin_to_city_["751"] = "bhubaneswar"; pin_to_city_["180"] = "jammu";
    pin_to_city_["160"] = "chandigarh";

    indian_state_normalize_["ka"]          = "karnataka";
    indian_state_normalize_["karnataka"]   = "karnataka";
    indian_state_normalize_["mh"]          = "maharashtra";
    indian_state_normalize_["maharashtra"] = "maharashtra";
    indian_state_normalize_["dl"]          = "delhi";
    indian_state_normalize_["delhi"]       = "delhi";
    indian_state_normalize_["new delhi"]   = "delhi";
    indian_state_normalize_["tn"]          = "tamil nadu";
    indian_state_normalize_["tamilnadu"]   = "tamil nadu";
    indian_state_normalize_["tamil nadu"]  = "tamil nadu";
    indian_state_normalize_["ap"]          = "andhra pradesh";
    indian_state_normalize_["ts"]          = "telangana";
    indian_state_normalize_["wb"]          = "west bengal";
    indian_state_normalize_["up"]          = "uttar pradesh";
    indian_state_normalize_["mp"]          = "madhya pradesh";
    indian_state_normalize_["rj"]          = "rajasthan";
    indian_state_normalize_["gj"]          = "gujarat";
    indian_state_normalize_["kl"]          = "kerala";
    indian_state_normalize_["pb"]          = "punjab";
    indian_state_normalize_["hr"]          = "haryana";
    indian_state_normalize_["jk"]          = "jammu and kashmir";
    indian_state_normalize_["uk"]          = "uttarakhand";
    indian_state_normalize_["or"]          = "odisha";
    indian_state_normalize_["orissa"]      = "odisha";
    indian_state_normalize_["br"]          = "bihar";
    indian_state_normalize_["jh"]          = "jharkhand";
    indian_state_normalize_["cg"]          = "chhattisgarh";
    indian_state_normalize_["chattisgarh"] = "chhattisgarh";
}

void RuleEngine::initUSStateData() {
    state_abbrev_to_full_["AL"] = "Alabama";     state_abbrev_to_full_["AK"] = "Alaska";
    state_abbrev_to_full_["AZ"] = "Arizona";     state_abbrev_to_full_["AR"] = "Arkansas";
    state_abbrev_to_full_["CA"] = "California";  state_abbrev_to_full_["CO"] = "Colorado";
    state_abbrev_to_full_["CT"] = "Connecticut"; state_abbrev_to_full_["DE"] = "Delaware";
    state_abbrev_to_full_["FL"] = "Florida";     state_abbrev_to_full_["GA"] = "Georgia";
    state_abbrev_to_full_["HI"] = "Hawaii";      state_abbrev_to_full_["ID"] = "Idaho";
    state_abbrev_to_full_["IL"] = "Illinois";    state_abbrev_to_full_["IN"] = "Indiana";
    state_abbrev_to_full_["IA"] = "Iowa";        state_abbrev_to_full_["KS"] = "Kansas";
    state_abbrev_to_full_["KY"] = "Kentucky";    state_abbrev_to_full_["LA"] = "Louisiana";
    state_abbrev_to_full_["ME"] = "Maine";       state_abbrev_to_full_["MD"] = "Maryland";
    state_abbrev_to_full_["MA"] = "Massachusetts"; state_abbrev_to_full_["MI"] = "Michigan";
    state_abbrev_to_full_["MN"] = "Minnesota";   state_abbrev_to_full_["MS"] = "Mississippi";
    state_abbrev_to_full_["MO"] = "Missouri";    state_abbrev_to_full_["MT"] = "Montana";
    state_abbrev_to_full_["NE"] = "Nebraska";    state_abbrev_to_full_["NV"] = "Nevada";
    state_abbrev_to_full_["NH"] = "New Hampshire"; state_abbrev_to_full_["NJ"] = "New Jersey";
    state_abbrev_to_full_["NM"] = "New Mexico";  state_abbrev_to_full_["NY"] = "New York";
    state_abbrev_to_full_["NC"] = "North Carolina"; state_abbrev_to_full_["ND"] = "North Dakota";
    state_abbrev_to_full_["OH"] = "Ohio";        state_abbrev_to_full_["OK"] = "Oklahoma";
    state_abbrev_to_full_["OR"] = "Oregon";      state_abbrev_to_full_["PA"] = "Pennsylvania";
    state_abbrev_to_full_["RI"] = "Rhode Island"; state_abbrev_to_full_["SC"] = "South Carolina";
    state_abbrev_to_full_["SD"] = "South Dakota"; state_abbrev_to_full_["TN"] = "Tennessee";
    state_abbrev_to_full_["TX"] = "Texas";       state_abbrev_to_full_["UT"] = "Utah";
    state_abbrev_to_full_["VT"] = "Vermont";     state_abbrev_to_full_["VA"] = "Virginia";
    state_abbrev_to_full_["WA"] = "Washington";  state_abbrev_to_full_["WV"] = "West Virginia";
    state_abbrev_to_full_["WI"] = "Wisconsin";   state_abbrev_to_full_["WY"] = "Wyoming";
    state_abbrev_to_full_["DC"] = "District of Columbia";
}

void RuleEngine::initMisspellings() {
    common_misspellings_["banglore"]    = "bengaluru";
    common_misspellings_["bangalore"]   = "bengaluru";
    common_misspellings_["bengalore"]   = "bengaluru";
    common_misspellings_["bombay"]      = "mumbai";
    common_misspellings_["calcutta"]    = "kolkata";
    common_misspellings_["madras"]      = "chennai";
    common_misspellings_["poona"]       = "pune";
    common_misspellings_["baroda"]      = "vadodara";
    common_misspellings_["trivandrum"]  = "thiruvananthapuram";
    common_misspellings_["cochin"]      = "kochi";
    common_misspellings_["mysore"]      = "mysuru";
    common_misspellings_["mangalore"]   = "mangaluru";
    common_misspellings_["pondicherry"] = "puducherry";
    common_misspellings_["gurgaon"]     = "gurugram";
    common_misspellings_["new york city"] = "new york";
    common_misspellings_["nyc"]         = "new york";
    common_misspellings_["la"]          = "los angeles";
    common_misspellings_["sf"]          = "san francisco";
    common_misspellings_["philly"]      = "philadelphia";
}

} // namespace addr
