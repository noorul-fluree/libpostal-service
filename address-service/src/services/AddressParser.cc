#include "services/AddressParser.h"
#include <libpostal/libpostal.h>
#include <iostream>
#include <cstring>

namespace addr {

std::once_flag AddressParser::init_flag_;

bool AddressParser::initialize(const std::string& data_dir) {
    bool success = false;
    std::call_once(init_flag_, [&]() {
        if (!data_dir.empty()) {
            setenv("LIBPOSTAL_DATA_DIR", data_dir.c_str(), 1);
        }

        if (!libpostal_setup()) {
            std::cerr << "[AddressParser] ERROR: Failed to initialize libpostal" << std::endl;
            return;
        }
        if (!libpostal_setup_parser()) {
            std::cerr << "[AddressParser] ERROR: Failed to initialize libpostal parser" << std::endl;
            return;
        }
        if (!libpostal_setup_language_classifier()) {
            std::cerr << "[AddressParser] ERROR: Failed to initialize language classifier" << std::endl;
            return;
        }

        std::cout << "[AddressParser] libpostal initialized successfully" << std::endl;
        success = true;
    });

    if (success) initialized_.store(true);
    return initialized_.load();
}

void AddressParser::shutdown() {
    if (initialized_.load()) {
        libpostal_teardown();
        libpostal_teardown_parser();
        libpostal_teardown_language_classifier();
        initialized_.store(false);
        std::cout << "[AddressParser] libpostal shut down" << std::endl;
    }
}

ParsedAddress AddressParser::parse(const std::string& address) const {
    ParsedAddress result;
    result.raw_input = address;

    if (!initialized_.load()) { result.error = "Parser not initialized"; return result; }
    if (address.empty())       { result.error = "Empty address";          return result; }

    libpostal_address_parser_options_t options = libpostal_get_address_parser_default_options();

    // FIX: libpostal wants char* not const char* — safe const_cast (libpostal does not modify input)
    libpostal_address_parser_response_t* response =
        libpostal_parse_address(const_cast<char*>(address.c_str()), options);

    if (!response) {
        result.error = "libpostal parse returned null";
        return result;
    }

    for (size_t i = 0; i < response->num_components; ++i) {
        const char* label = response->labels[i];
        const char* value = response->components[i];

        if      (strcmp(label, "house_number")  == 0) result.house_number  = value;
        else if (strcmp(label, "road")          == 0) result.road          = value;
        else if (strcmp(label, "suburb")        == 0) result.suburb        = value;
        else if (strcmp(label, "city")          == 0) result.city          = value;
        else if (strcmp(label, "city_district") == 0) result.city_district = value;
        else if (strcmp(label, "state")         == 0) result.state         = value;
        else if (strcmp(label, "state_district")== 0) result.state_district= value;
        else if (strcmp(label, "postcode")      == 0) result.postcode      = value;
        else if (strcmp(label, "country")       == 0) result.country       = value;
        else if (strcmp(label, "unit")          == 0) result.unit          = value;
        else if (strcmp(label, "level")         == 0) result.level         = value;
        else if (strcmp(label, "staircase")     == 0) result.staircase     = value;
        else if (strcmp(label, "entrance")      == 0) result.entrance      = value;
        else if (strcmp(label, "po_box")        == 0) result.po_box        = value;
    }

    libpostal_address_parser_response_destroy(response);
    return result;
}

NormalizedAddress AddressParser::normalize(const std::string& address) const {
    NormalizedAddress result;
    result.raw_input = address;

    if (!initialized_.load() || address.empty()) return result;

    libpostal_normalize_options_t options = libpostal_get_default_options();
    size_t num_expansions = 0;

    // FIX: same const_cast for libpostal_expand_address
    char** expansions = libpostal_expand_address(
        const_cast<char*>(address.c_str()), options, &num_expansions);

    if (expansions) {
        for (size_t i = 0; i < num_expansions; ++i)
            result.normalizations.emplace_back(expansions[i]);
        libpostal_expansion_array_destroy(expansions, num_expansions);
    }

    return result;
}

} // namespace addr