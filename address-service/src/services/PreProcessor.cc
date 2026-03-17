#include "services/PreProcessor.h"
#include <sstream>
#include <cctype>

namespace addr {

PreProcessor::PreProcessor() {
    initAbbreviations();
}

std::string PreProcessor::process(const std::string& raw) const {
    std::string result = raw;
    result = trimWhitespace(result);
    result = removeJunkChars(result);
    result = normalizeCase(result);
    result = expandAbbreviations(result);
    result = collapseWhitespace(result);
    result = trimWhitespace(result);
    return result;
}

std::string PreProcessor::trimWhitespace(const std::string& s) const {
    size_t start = s.find_first_not_of(" \t\n\r\f\v");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\n\r\f\v");
    return s.substr(start, end - start + 1);
}

std::string PreProcessor::normalizeUTF8(const std::string& s) const {
    // Replace common non-ASCII characters with ASCII equivalents
    std::string result;
    result.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {
            result += s[i];
        } else if (c == 0xC2 && i + 1 < s.size()) {
            unsigned char next = static_cast<unsigned char>(s[i + 1]);
            if (next == 0xB0) { // degree symbol
                result += " ";
                ++i;
            } else {
                result += s[i];
            }
        } else {
            result += s[i]; // pass through other UTF-8
        }
    }
    return result;
}

std::string PreProcessor::removeJunkChars(const std::string& s) const {
    std::string result;
    result.reserve(s.size());
    for (char c : s) {
        // Keep alphanumeric, spaces, commas, periods, hyphens, slashes, hash, apostrophe
        if (std::isalnum(static_cast<unsigned char>(c)) ||
            c == ' ' || c == ',' || c == '.' || c == '-' ||
            c == '/' || c == '#' || c == '\'' || c == '&' ||
            c == '(' || c == ')' ||
            static_cast<unsigned char>(c) >= 0x80) { // preserve UTF-8
            result += c;
        } else if (c == '\t' || c == '\n' || c == '\r') {
            result += ' '; // newlines/tabs become spaces
        }
        // Other characters are silently dropped
    }
    return result;
}

std::string PreProcessor::collapseWhitespace(const std::string& s) const {
    std::string result;
    result.reserve(s.size());
    bool last_was_space = false;
    for (char c : s) {
        if (c == ' ') {
            if (!last_was_space) {
                result += c;
                last_was_space = true;
            }
        } else {
            result += c;
            last_was_space = false;
        }
    }
    return result;
}

std::string PreProcessor::normalizeCase(const std::string& s) const {
    std::string result = s;
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return result;
}

std::string PreProcessor::expandAbbreviations(const std::string& s) const {
    // Tokenize by spaces, replace known abbreviations
    std::istringstream iss(s);
    std::string token;
    std::string result;
    bool first = true;

    while (iss >> token) {
        if (!first) result += ' ';
        first = false;

        // Remove trailing punctuation for lookup
        std::string clean = token;
        std::string trailing;
        while (!clean.empty() && (clean.back() == ',' || clean.back() == '.')) {
            trailing = clean.back() + trailing;
            clean.pop_back();
        }

        auto it = abbreviations_.find(clean);
        if (it != abbreviations_.end()) {
            result += it->second;
            // Re-add trailing comma if present
            if (!trailing.empty() && trailing.find(',') != std::string::npos) {
                result += ',';
            }
        } else {
            result += token;
        }
    }
    return result;
}

void PreProcessor::initAbbreviations() {
    // Common street abbreviations
    abbreviations_["st"]     = "street";
    abbreviations_["st."]    = "street";
    abbreviations_["ave"]    = "avenue";
    abbreviations_["ave."]   = "avenue";
    abbreviations_["blvd"]   = "boulevard";
    abbreviations_["blvd."]  = "boulevard";
    abbreviations_["dr"]     = "drive";
    abbreviations_["dr."]    = "drive";
    abbreviations_["rd"]     = "road";
    abbreviations_["rd."]    = "road";
    abbreviations_["ln"]     = "lane";
    abbreviations_["ln."]    = "lane";
    abbreviations_["ct"]     = "court";
    abbreviations_["ct."]    = "court";
    abbreviations_["pl"]     = "place";
    abbreviations_["pl."]    = "place";
    abbreviations_["cir"]    = "circle";
    abbreviations_["cir."]   = "circle";
    abbreviations_["pkwy"]   = "parkway";
    abbreviations_["hwy"]    = "highway";
    abbreviations_["expy"]   = "expressway";
    abbreviations_["tpke"]   = "turnpike";
    abbreviations_["fwy"]    = "freeway";

    // Directional
    abbreviations_["n"]   = "north";
    abbreviations_["n."]  = "north";
    abbreviations_["s"]   = "south";
    abbreviations_["s."]  = "south";
    abbreviations_["e"]   = "east";
    abbreviations_["e."]  = "east";
    abbreviations_["w"]   = "west";
    abbreviations_["w."]  = "west";
    abbreviations_["ne"]  = "northeast";
    abbreviations_["nw"]  = "northwest";
    abbreviations_["se"]  = "southeast";
    abbreviations_["sw"]  = "southwest";

    // Unit types
    abbreviations_["apt"]   = "apartment";
    abbreviations_["apt."]  = "apartment";
    abbreviations_["ste"]   = "suite";
    abbreviations_["ste."]  = "suite";
    abbreviations_["flr"]   = "floor";
    abbreviations_["flr."]  = "floor";
    abbreviations_["bldg"]  = "building";
    abbreviations_["bldg."] = "building";
    abbreviations_["dept"]  = "department";
    abbreviations_["dept."] = "department";
    abbreviations_["rm"]    = "room";
    abbreviations_["rm."]   = "room";
    abbreviations_["blk"]   = "block";
    abbreviations_["blk."]  = "block";

    // India-specific
    abbreviations_["nagar"] = "nagar";
    abbreviations_["ngr"]   = "nagar";
    abbreviations_["marg"]  = "marg";
    abbreviations_["mg"]    = "mahatma gandhi";
    abbreviations_["jn"]    = "junction";
    abbreviations_["jn."]   = "junction";
    abbreviations_["stn"]   = "station";
    abbreviations_["stn."]  = "station";
    abbreviations_["dist"]  = "district";
    abbreviations_["dist."] = "district";
    abbreviations_["opp"]   = "opposite";
    abbreviations_["opp."]  = "opposite";
    abbreviations_["nr"]    = "near";
    abbreviations_["nr."]   = "near";

    // Country/region
    abbreviations_["us"]  = "united states";
    abbreviations_["usa"] = "united states";
    abbreviations_["uk"]  = "united kingdom";
    abbreviations_["uae"] = "united arab emirates";
}

} // namespace addr
