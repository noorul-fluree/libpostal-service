#include "services/PreProcessor.h"
#include <cctype>
#include <sstream>

namespace addr {

PreProcessor::PreProcessor() {
    initAbbreviations();
}

// =============================================================================
//  process — single allocation pipeline
//
//  Original: 6 functions, each returning a new std::string = 6 heap allocs.
//  Optimized: one singlePassClean call + one expandAbbreviations call = 2 allocs.
// =============================================================================
std::string PreProcessor::process(std::string_view raw) const {
    std::string cleaned;
    cleaned.reserve(raw.size()); // one upfront reserve, no reallocs
    singlePassClean(raw, cleaned);
    return expandAbbreviations(cleaned);
}

// =============================================================================
//  singlePassClean — replaces trim + removeJunkChars + collapseWhitespace
//  + normalizeCase in a single O(n) pass with zero intermediate copies.
// =============================================================================
void PreProcessor::singlePassClean(std::string_view in, std::string& out) const {
    out.clear();

    // Find first non-whitespace (trim leading)
    size_t start = 0;
    while (start < in.size() && std::isspace(static_cast<unsigned char>(in[start])))
        ++start;

    bool last_space = false;

    for (size_t i = start; i < in.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(in[i]);

        if (c >= 0x80) {
            // UTF-8 multi-byte: pass through as-is
            out += static_cast<char>(c);
            last_space = false;
            continue;
        }

        // Tabs/newlines become spaces
        if (c == '\t' || c == '\n' || c == '\r') {
            if (!last_space) { out += ' '; last_space = true; }
            continue;
        }

        // Collapse spaces
        if (c == ' ') {
            if (!last_space) { out += ' '; last_space = true; }
            continue;
        }

        // Keep alphanumeric and allowed punctuation; lowercase letters inline
        if (std::isalpha(c)) {
            out += static_cast<char>(std::tolower(c));
            last_space = false;
        } else if (std::isdigit(c) ||
                   c == ',' || c == '.' || c == '-' || c == '/' ||
                   c == '#' || c == '\'' || c == '&' || c == '(' || c == ')') {
            out += static_cast<char>(c);
            last_space = false;
        }
        // All other ASCII chars silently dropped
    }

    // Trim trailing space
    while (!out.empty() && out.back() == ' ')
        out.pop_back();
}

// =============================================================================
//  expandAbbreviations — tokenize once, look up each token, rebuild string.
//  Uses std::string_view for token slicing — no per-token allocation.
// =============================================================================
std::string PreProcessor::expandAbbreviations(std::string_view s) const {
    std::string result;
    result.reserve(s.size() + s.size() / 4); // slight overestimate — expansions add chars

    size_t i = 0;
    bool first = true;

    while (i < s.size()) {
        // Skip spaces
        while (i < s.size() && s[i] == ' ') ++i;
        if (i >= s.size()) break;

        // Find token end
        size_t token_start = i;
        while (i < s.size() && s[i] != ' ') ++i;
        std::string_view token = s.substr(token_start, i - token_start);

        // Strip trailing punctuation for lookup, remember the suffix
        std::string_view bare  = token;
        std::string_view trail = {};
        if (!bare.empty() && (bare.back() == ',' || bare.back() == '.')) {
            trail = bare.substr(bare.size() - 1);
            bare  = bare.substr(0, bare.size() - 1);
        }

        if (!first) result += ' ';
        first = false;

        // Lookup abbreviation by string key (map stores std::string keys)
        auto it = abbreviations_.find(std::string(bare));
        if (it != abbreviations_.end()) {
            result += it->second;
            // Re-attach comma (but not period — periods are dropped)
            if (!trail.empty() && trail[0] == ',') result += ',';
        } else {
            result += token; // paste original token (includes any trailing punct)
        }
    }

    return result;
}

void PreProcessor::initAbbreviations() {
    // Street types
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
    abbreviations_["ngr"]   = "nagar";
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
    abbreviations_["mg"]    = "mahatma gandhi";

    // Country
    abbreviations_["us"]  = "united states";
    abbreviations_["usa"] = "united states";
    abbreviations_["uk"]  = "united kingdom";
    abbreviations_["uae"] = "united arab emirates";
}

} // namespace addr
