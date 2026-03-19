#pragma once

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <algorithm>

namespace addr {

// =============================================================================
//  PreProcessor — optimized address string cleaner
//
//  Key optimizations vs original:
//   1. Single-pass pipeline — one output buffer, no intermediate string copies
//   2. string_view inputs everywhere — zero-copy reads from caller's buffer
//   3. Abbreviation lookup on string_view keys — avoids alloc for each token
//   4. No std::regex — all matching is hand-rolled character-level logic
//   5. reserve() on output buffer using input size as hint
// =============================================================================

class PreProcessor {
public:
    PreProcessor();

    // Main entry: clean and pre-process a raw address string.
    // Returns a new string (one allocation for the entire pipeline).
    std::string process(std::string_view raw) const;

private:
    // Single-pass processor: trim + remove junk + collapse whitespace + lowercase
    // Written into `out` in one pass over `in`.
    void singlePassClean(std::string_view in, std::string& out) const;

    // Expand abbreviations in-place on an already-lowercased, cleaned string.
    // Tokenizes once, replaces known tokens. Fewer allocs than the original.
    std::string expandAbbreviations(std::string_view s) const;

    // Lookup table: lowercase token -> expanded form
    // Using std::string keys but looking up via string_view to avoid alloc
    std::unordered_map<std::string, std::string> abbreviations_;

    void initAbbreviations();
};

} // namespace addr
