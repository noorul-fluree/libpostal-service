#include "services/AddressParser.h"
#include <libpostal/libpostal.h>
#include <iostream>

namespace addr {

std::once_flag AddressParser::init_flag_;

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
//  initialize
//  FIX: use libpostal_setup_datadir() instead of libpostal_setup() so the
//       data directory is passed explicitly. libpostal_setup() ignores the
//       LIBPOSTAL_DATA_DIR env var in this build — _datadir() variants are
//       the correct API for runtime-configurable data paths.
// =============================================================================
bool AddressParser::initialize(const std::string& data_dir) {
    bool success = false;

    std::call_once(init_flag_, [&]() {
        // Store data_dir as member so initLanguageClassifier() can use it
        data_dir_ = data_dir;

        const char* dir = data_dir_.empty() ? nullptr
                                            : data_dir_.c_str();

        // Use _datadir() variants — explicitly pass the path instead of
        // relying on env var which libpostal_setup() ignores in v1.1.4
        if (!libpostal_setup_datadir(const_cast<char*>(dir))) {
            std::cerr << "[AddressParser] ERROR: libpostal_setup_datadir() failed"
                      << " (dir=" << (dir ? dir : "null") << ")\n";
            return;
        }
        if (!libpostal_setup_parser_datadir(const_cast<char*>(dir))) {
            std::cerr << "[AddressParser] ERROR: libpostal_setup_parser_datadir() failed\n";
            libpostal_teardown();
            return;
        }

        std::cout << "[AddressParser] libpostal ready"
                  << " | data_dir=" << (dir ? dir : "(default)")
                  << " | parser only, no classifier\n";
        success = true;
    });

    if (success) initialized_.store(true);
    return initialized_.load();
}

// =============================================================================
//  initLanguageClassifier
//  FIX: use libpostal_setup_language_classifier_datadir() — same reason as
//       above, explicit path instead of env var.
// =============================================================================
bool AddressParser::initLanguageClassifier() {
    if (classifier_ready_.load()) return true;

    const char* dir = data_dir_.empty() ? nullptr : data_dir_.c_str();

    if (!libpostal_setup_language_classifier_datadir(const_cast<char*>(dir))) {
        std::cerr << "[AddressParser] ERROR: libpostal_setup_language_classifier_datadir() failed\n";
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
//  shutdown
// =============================================================================
void AddressParser::shutdown() {
    if (!initialized_.load()) return;
    teardownLanguageClassifier();
    libpostal_teardown_parser();
    libpostal_teardown();
    initialized_.store(false);
    std::cout << "[AddressParser] libpostal shut down\n";
}

// =============================================================================
//  parse
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

    static const libpostal_address_parser_options_t kDefaultOptions =
        libpostal_get_address_parser_default_options();

    libpostal_address_parser_options_t options = kDefaultOptions;
    options.language = language.empty() ? nullptr : const_cast<char*>(language.c_str());
    options.country  = country.empty()  ? nullptr : const_cast<char*>(country.c_str());

    libpostal_address_parser_response_t* response = nullptr;
    {
        std::lock_guard<std::mutex> lock(parseMutex(address));
        response = libpostal_parse_address(
            const_cast<char*>(address.c_str()), options);
    }

    if (!response) {
        result.error = "libpostal parse returned null";
        return result;
    }

    const auto& lmap = labelMap();
    for (size_t i = 0; i < response->num_components; ++i) {
        auto it = lmap.find(std::string_view(response->labels[i]));
        if (it != lmap.end())
            result.*(it->second) = response->components[i];
    }

    libpostal_address_parser_response_destroy(response);
    return result;
}

// =============================================================================
//  normalize
// =============================================================================
NormalizedAddress AddressParser::normalize(const std::string& address,
                                            const std::string& language,
                                            const std::string& country) const {
    NormalizedAddress result;
    result.raw_input = address;

    if (!initialized_.load(std::memory_order_relaxed) || address.empty())
        return result;

    static const libpostal_normalize_options_t kDefaultNormOpts =
        libpostal_get_default_options();

    libpostal_normalize_options_t options = kDefaultNormOpts;

    const char* lang_arr[1];
    if (!language.empty()) {
        lang_arr[0]           = language.c_str();
        options.languages     = const_cast<char**>(lang_arr);
        options.num_languages = 1;
    }

    size_t num_expansions = 0;
    char** expansions     = nullptr;
    {
        std::lock_guard<std::mutex> lock(expandMutex(address));
        expansions = libpostal_expand_address(
            const_cast<char*>(address.c_str()), options, &num_expansions);
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
//  deduplicateBatch
// =============================================================================
AddressParser::DedupeResult
AddressParser::deduplicateBatch(const std::vector<std::string>& addresses) const {
    const size_t N = addresses.size();
    DedupeResult result;
    result.canonical_index.resize(N, -1);

    if (N == 0) return result;

    if (classifier_ready_.load(std::memory_order_relaxed)) {
        std::unordered_map<std::string, int> hash_to_canonical;
        hash_to_canonical.reserve(N);

        for (size_t i = 0; i < N; ++i) {
            const auto& addr = addresses[i];
            if (addr.empty()) {
                result.canonical_index[i] = static_cast<int>(i);
                result.unique_indices.push_back(i);
                continue;
            }

            const char* labels[] = {"house"};
            const char* values[] = {addr.c_str()};

            size_t num_hashes = 0;
            char** hashes     = nullptr;
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
                key = hashes[0];
                libpostal_expansion_array_destroy(hashes, num_hashes);
            } else {
                key = addr;
            }

            auto [it, inserted] = hash_to_canonical.emplace(key, static_cast<int>(i));
            if (inserted) {
                result.canonical_index[i] = static_cast<int>(i);
                result.unique_indices.push_back(i);
            } else {
                result.canonical_index[i] = it->second;
            }
        }
    } else {
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