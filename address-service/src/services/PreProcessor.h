#pragma once

#include <string>
#include <unordered_map>
#include <vector>
#include <algorithm>
#include <regex>

namespace addr {

class PreProcessor {
public:
    PreProcessor();

    // Main entry: clean and pre-process a raw address string
    std::string process(const std::string& raw) const;

private:
    // Individual cleaning steps
    std::string trimWhitespace(const std::string& s) const;
    std::string normalizeUTF8(const std::string& s) const;
    std::string removeJunkChars(const std::string& s) const;
    std::string collapseWhitespace(const std::string& s) const;
    std::string expandAbbreviations(const std::string& s) const;
    std::string normalizeCase(const std::string& s) const;

    // Abbreviation map
    std::unordered_map<std::string, std::string> abbreviations_;

    void initAbbreviations();
};

} // namespace addr
