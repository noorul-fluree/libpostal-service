#include "services/PreProcessor.h"
#include <iostream>
#include <stdexcept>
#include <functional>

extern void registerTest(const std::string& name, std::function<void()> fn);

#define ASSERT_EQ_STR(a, b) \
    if ((a) != (b)) { \
        throw std::runtime_error(std::string("Expected: '") + (b) + "' Got: '" + (a) + "'"); \
    }

void register_preprocessor_tests() {
    static addr::PreProcessor pp;

    registerTest("PreProcessor::trimWhitespace", []() {
        ASSERT_EQ_STR(pp.process("  hello  "), "hello");
        ASSERT_EQ_STR(pp.process("\t 123 main st \n"), "123 main street");
    });

    registerTest("PreProcessor::normalizeCase", []() {
        ASSERT_EQ_STR(pp.process("HELLO WORLD"), "hello world");
        ASSERT_EQ_STR(pp.process("New York"), "new york");
    });

    registerTest("PreProcessor::collapseWhitespace", []() {
        ASSERT_EQ_STR(pp.process("123   main    st"), "123 main street");
    });

    registerTest("PreProcessor::removeJunkChars", []() {
        // Keeps alphanumeric, commas, periods, hyphens, hash
        ASSERT_EQ_STR(pp.process("123 Main St. #4A"), "123 main street #4a");
    });

    registerTest("PreProcessor::expandAbbreviations_street", []() {
        ASSERT_EQ_STR(pp.process("123 Main St"), "123 main street");
        ASSERT_EQ_STR(pp.process("456 Park Ave"), "456 park avenue");
        ASSERT_EQ_STR(pp.process("789 Oak Blvd"), "789 oak boulevard");
        ASSERT_EQ_STR(pp.process("100 Pine Dr"), "100 pine drive");
        ASSERT_EQ_STR(pp.process("200 Elm Rd"), "200 elm road");
    });

    registerTest("PreProcessor::expandAbbreviations_directional", []() {
        ASSERT_EQ_STR(pp.process("N Main St"), "north main street");
        ASSERT_EQ_STR(pp.process("123 S Broadway"), "123 south broadway");
    });

    registerTest("PreProcessor::expandAbbreviations_unit", []() {
        ASSERT_EQ_STR(pp.process("Apt 4B"), "apartment 4b");
        ASSERT_EQ_STR(pp.process("Ste 100"), "suite 100");
        ASSERT_EQ_STR(pp.process("Bldg 3"), "building 3");
    });

    registerTest("PreProcessor::expandAbbreviations_india", []() {
        ASSERT_EQ_STR(pp.process("MG Road"), "mahatma gandhi road");
        ASSERT_EQ_STR(pp.process("Stn Rd"), "station road");
        ASSERT_EQ_STR(pp.process("Opp Railway Stn"), "opposite railway station");
    });

    registerTest("PreProcessor::emptyInput", []() {
        ASSERT_EQ_STR(pp.process(""), "");
        ASSERT_EQ_STR(pp.process("   "), "");
    });

    registerTest("PreProcessor::preservesCommas", []() {
        std::string result = pp.process("123 Main St, New York, NY");
        // Should contain commas
        if (result.find(',') == std::string::npos) {
            throw std::runtime_error("Commas were removed: " + result);
        }
    });

    registerTest("PreProcessor::fullAddress_indian", []() {
        std::string result = pp.process("  123, MG Rd, Blk 5, Nr Railway Stn, Bengaluru 560001  ");
        // Should expand abbreviations and clean up
        if (result.find("mahatma gandhi") == std::string::npos) {
            throw std::runtime_error("MG not expanded: " + result);
        }
        if (result.find("near") == std::string::npos) {
            throw std::runtime_error("Nr not expanded: " + result);
        }
    });

    registerTest("PreProcessor::fullAddress_us", []() {
        std::string result = pp.process("456 N Park Ave, Apt 3B, New York, NY 10022");
        if (result.find("north") == std::string::npos) {
            throw std::runtime_error("N not expanded: " + result);
        }
        if (result.find("avenue") == std::string::npos) {
            throw std::runtime_error("Ave not expanded: " + result);
        }
    });
}
