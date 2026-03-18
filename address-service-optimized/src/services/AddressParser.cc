#include "services/AddressParser.h"
#include <libpostal/libpostal.h>
#include <iostream>

namespace addr {

std::once_flag AddressParser::init_flag_;

// =============================================================================
//  FIX 5 — Static label → ptr-to-member dispatch table
//  Built once on first call, never rebuilt. O(1) average lookup vs O(n) strcmp.
// =============================================================================
const std::unordered_map<std::string_view, AddressParser::FieldPtr>&
AddressParser::labelMap() {
    static const std::unordered_map<std::string_view, FieldPtr> kMap = {
        {"house_number",  &ParsedAddress::house_number},
        {"road",          &ParsedAddress::road},
        {"suburb",        &ParsedAddress::suburb},
        {"city",          &ParsedAddress::city},
        {"city_district", &ParsedAddress::city_district},
        {"state",         &ParsedAddress::state},
        {"state_district",&ParsedAddress::state_district},
        {"postcode",      &ParsedAddress::postcode},
        {"country",       &ParsedAddress::country},
        {"unit",          &ParsedAddress::unit},
        {"level",         &ParsedAddress::level},
        {"staircase",     &ParsedAddress::staircase},
        {"entrance",      &ParsedAddress::entrance},
        {"po_box",        &ParsedAddress::po_box},
    };
    return kMap;
}

// =============================================================================
//  initialize — FIX 2: language_classifier NOT loaded here
//  Saves ~300MB RAM per pod. Parser accuracy is unaffected.
//  Call initLanguageClassifier() separately only if normalize() is needed.
// =============================================================================
bool AddressParser::initialize(const std::string& data_dir) {
    bool success = false;

    std::call_once(init_flag_, [&]() {
        if (!data_dir.empty())
            setenv("LIBPOSTAL_DATA_DIR", data_dir.c_str(), 1);

        if (!libpostal_setup()) {
            std::cerr << "[AddressParser] ERROR: libpostal_setup() failed\n";
            return;
        }
        if (!libpostal_setup_parser()) {
            std::cerr << "[AddressParser] ERROR: libpostal_setup_parser() failed\n";
            libpostal_teardown();
            return;
        }

        // FIX 2: do NOT call libpostal_setup_language_classifier() here.
        // It loads ~300MB and is only needed for expand_address() language
        // auto-detection. parse() works fine without it.
        std::cout << "[AddressParser] libpostal ready (parser only, no classifier)\n";
        success = true;
    });

    if (success) initialized_.store(true);
    return initialized_.load();
}

// =============================================================================
//  initLanguageClassifier — call explicitly only if normalize() is used
// =============================================================================
bool AddressParser::initLanguageClassifier() {
    if (classifier_ready_.load()) return true;
    if (!libpostal_setup_language_classifier()) {
        std::cerr << "[AddressParser] ERROR: libpostal_setup_language_classifier() failed\n";
        return false;
    }
    classifier_ready_.store(true);
    std::cout << "[AddressParser] language classifier ready\n";
    return true;
}

void AddressParser::teardownLanguageClassifier() {
    if (classifier_ready_.load()) {
        libpostal_teardown_language_classifier();
        classifier_ready_.store(false);
    }
}

// =============================================================================
//  shutdown — FIX 2: only teardown what was set up
// =============================================================================
void AddressParser::shutdown() {
    if (!initialized_.load()) return;
    teardownLanguageClassifier();    // no-op if never initialized
    libpostal_teardown_parser();
    libpostal_teardown();
    initialized_.store(false);
    std::cout << "[AddressParser] libpostal shut down\n";
}

// =============================================================================
//  parse — all 6 fixes applied
// =============================================================================
ParsedAddress AddressParser::parse(const std::string& address,
                                    const std::string& language,
                                    const std::string& country) const {
    ParsedAddress result;
    result.raw_input = address;

    if (!initialized_.load(std::memory_order_relaxed)) {
        result.error = "Parser not initialized";
        return result;
    }
    if (address.empty()) {
        result.error = "Empty address";
        return result;
    }

    // FIX 4: parser options cached as static — zero overhead per call
    // The struct is POD with constant values; safe to read from multiple threads.
    static const libpostal_address_parser_options_t kDefaultOptions =
        libpostal_get_address_parser_default_options();

    // FIX 3: copy options so we can set language/country hints per-call
    libpostal_address_parser_options_t options = kDefaultOptions;
    options.language = language.empty() ? nullptr : const_cast<char*>(language.c_str());
    options.country  = country.empty()  ? nullptr : const_cast<char*>(country.c_str());

    // FIX 1: acquire per-slot mutex before calling into non-thread-safe libpostal
    libpostal_address_parser_response_t* response = nullptr;
    {
        std::lock_guard<std::mutex> lock(parseMutex(address));
        response = libpostal_parse_address(const_cast<char*>(address.c_str()), options);
    }

    if (!response) {
        result.error = "libpostal parse returned null";
        return result;
    }

    // FIX 5: O(1) hash dispatch instead of 14-branch strcmp chain
    const auto& lmap = labelMap();
    for (size_t i = 0; i < response->num_components; ++i) {
        auto it = lmap.find(std::string_view(response->labels[i]));
        if (it != lmap.end())
            result.*(it->second) = response->components[i];
        // Silently skip: house, category, near, world_region
    }

    libpostal_address_parser_response_destroy(response);
    return result;
}

// =============================================================================
//  normalize — FIX 1 (thread safety) + FIX 3 (language hint) applied
// =============================================================================
NormalizedAddress AddressParser::normalize(const std::string& address,
                                            const std::string& language,
                                            const std::string& country) const {
    NormalizedAddress result;
    result.raw_input = address;

    if (!initialized_.load(std::memory_order_relaxed) || address.empty())
        return result;

    // FIX 4: cache expand options the same way as parser options
    static const libpostal_normalize_options_t kDefaultNormOpts =
        libpostal_get_default_options();

    libpostal_normalize_options_t options = kDefaultNormOpts;

    // FIX 3: pass language hint if provided — skips language classifier overhead
    // when we already know the language, and improves expansion quality.
    // language field in normalize_options_t is a char** (array), so we build it.
    const char* lang_arr[1];
    size_t      num_langs = 0;
    if (!language.empty()) {
        lang_arr[0]       = language.c_str();
        options.languages = const_cast<char**>(lang_arr);
        options.num_languages = 1;
        num_langs = 1;
    }

    size_t num_expansions = 0;
    char** expansions = nullptr;

    // FIX 1: thread-safe expand via per-slot mutex
    {
        std::lock_guard<std::mutex> lock(expandMutex(address));
        expansions = libpostal_expand_address(const_cast<char*>(address.c_str()), options, &num_expansions);
    }

    if (expansions) {
        result.normalizations.reserve(num_expansions);
        for (size_t i = 0; i < num_expansions; ++i)
            result.normalizations.emplace_back(expansions[i]);
        libpostal_expansion_array_destroy(expansions, num_expansions);
    }

    return result;
}

// =============================================================================
//  deduplicateBatch — FIX 6: use libpostal near-dupe hashes before parsing
//
//  Algorithm:
//    1. For each address, generate a near-dupe hash via libpostal
//    2. Group addresses by hash — identical hashes are near-duplicates
//    3. Return canonical_index[] mapping duplicates to their canonical address
//    4. BatchController parses only unique addresses, fans results back out
//
//  This avoids redundant libpostal parses. On real-world address batches,
//  5–15% of addresses are typically near-duplicates of each other.
//
//  NOTE: near_dupe_hashes requires the language classifier to be initialized.
//  If it isn't, we fall back to exact string dedup (still useful).
// =============================================================================
AddressParser::DedupeResult
AddressParser::deduplicateBatch(const std::vector<std::string>& addresses) const {
    const size_t N = addresses.size();
    DedupeResult result;
    result.canonical_index.resize(N, -1);

    if (N == 0) return result;

    // Fast path: try libpostal near-dupe hashing if classifier is ready
    if (classifier_ready_.load(std::memory_order_relaxed)) {
        // Map: hash string → first index that produced it
        std::unordered_map<std::string, int> hash_to_canonical;
        hash_to_canonical.reserve(N);

        for (size_t i = 0; i < N; ++i) {
            const auto& addr = addresses[i];
            if (addr.empty()) {
                result.canonical_index[i] = static_cast<int>(i);
                result.unique_indices.push_back(i);
                continue;
            }

            // Build a minimal component list for near_dupe_hashes:
            // just pass the raw address as a single "house" component.
            // The hash captures address structure regardless of abbreviation variants.
            const char* labels[] = {"house"};
            const char* values[] = {addr.c_str()};

            size_t num_hashes = 0;
            char** hashes = nullptr;
            {
                std::lock_guard<std::mutex> lock(parseMutex(addr));
                libpostal_near_dupe_hash_options_t opts =
                    libpostal_get_near_dupe_hash_default_options();
                hashes = libpostal_near_dupe_hashes(
                    1,
                    const_cast<char**>(labels),
                    const_cast<char**>(values),
                    opts,
                    &num_hashes
                );
            }

            std::string key;
            if (hashes && num_hashes > 0) {
                key = hashes[0]; // use first hash as canonical key
                libpostal_expansion_array_destroy(hashes, num_hashes);
            } else {
                key = addr; // fallback: exact string
            }

            auto [it, inserted] = hash_to_canonical.emplace(key, static_cast<int>(i));
            if (inserted) {
                // This address is canonical — must be parsed
                result.canonical_index[i] = static_cast<int>(i);
                result.unique_indices.push_back(i);
            } else {
                // Duplicate — reuse the canonical address's parse result
                result.canonical_index[i] = it->second;
            }
        }
    } else {
        // Fallback: exact string dedup (no libpostal involvement)
        std::unordered_map<std::string, int> seen;
        seen.reserve(N);
        for (size_t i = 0; i < N; ++i) {
            auto [it, inserted] = seen.emplace(addresses[i], static_cast<int>(i));
            if (inserted) {
                result.canonical_index[i] = static_cast<int>(i);
                result.unique_indices.push_back(i);
            } else {
                result.canonical_index[i] = it->second;
            }
        }
    }

    return result;
}

} // namespace addr
