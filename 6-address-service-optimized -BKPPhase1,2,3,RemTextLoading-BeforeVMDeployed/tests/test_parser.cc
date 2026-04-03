#include "services/AddressParser.h"
#include <iostream>
#include <stdexcept>
#include <functional>

extern void registerTest(const std::string& name, std::function<void()> fn);

void register_parser_tests() {
    // Note: These tests require libpostal to be installed and data downloaded.
    // They are skipped by default in test_main.cc

    static addr::AddressParser parser;
    static bool initialized = false;

    registerTest("Parser::initialize", [&]() {
        const char* data_dir = std::getenv("LIBPOSTAL_DATA_DIR");
        std::string dir = data_dir ? data_dir : "/usr/share/libpostal";
        initialized = parser.initialize(dir);
        if (!initialized) {
            throw std::runtime_error("Failed to initialize libpostal (is data downloaded?)");
        }
    });

    registerTest("Parser::parse_us_address", [&]() {
        if (!initialized) throw std::runtime_error("Parser not initialized");
        auto result = parser.parse("123 Main Street, New York, NY 10001");
        if (result.house_number.empty()) throw std::runtime_error("Missing house_number");
        if (result.road.empty()) throw std::runtime_error("Missing road");
        if (result.city.empty() && result.state.empty())
            throw std::runtime_error("Missing city or state");
    });

    registerTest("Parser::parse_indian_address", [&]() {
        if (!initialized) throw std::runtime_error("Parser not initialized");
        auto result = parser.parse("42 MG Road, Bengaluru, Karnataka 560001");
        if (result.error.empty() == false) throw std::runtime_error("Parse error: " + result.error);
        // libpostal should extract at least some components
        int fields = 0;
        if (!result.house_number.empty()) ++fields;
        if (!result.road.empty()) ++fields;
        if (!result.city.empty()) ++fields;
        if (!result.postcode.empty()) ++fields;
        if (fields < 2) throw std::runtime_error("Too few components parsed: " + std::to_string(fields));
    });

    registerTest("Parser::parse_empty_address", [&]() {
        if (!initialized) throw std::runtime_error("Parser not initialized");
        auto result = parser.parse("");
        if (result.error.empty()) throw std::runtime_error("Expected error for empty input");
    });

    registerTest("Parser::normalize", [&]() {
        if (!initialized) throw std::runtime_error("Parser not initialized");
        auto result = parser.normalize("123 Main St New York NY");
        if (result.normalizations.empty()) {
            throw std::runtime_error("No normalizations returned");
        }
    });
}
