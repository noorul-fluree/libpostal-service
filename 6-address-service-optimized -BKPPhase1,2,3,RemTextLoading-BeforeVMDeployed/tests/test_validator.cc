#include "utils/InputValidator.h"
#include <iostream>
#include <stdexcept>
#include <functional>

extern void registerTest(const std::string& name, std::function<void()> fn);

void register_validator_tests() {

    registerTest("Validator::validAddress", []() {
        auto r = addr::InputValidator::validateAddress("123 Main Street, New York, NY 10001");
        if (!r.valid) throw std::runtime_error("Should be valid: " + r.error);
    });

    registerTest("Validator::emptyAddress", []() {
        auto r = addr::InputValidator::validateAddress("");
        if (r.valid) throw std::runtime_error("Empty should be invalid");
    });

    registerTest("Validator::tooShort", []() {
        auto r = addr::InputValidator::validateAddress("ab");
        if (r.valid) throw std::runtime_error("2-char should be invalid");
    });

    registerTest("Validator::tooLong", []() {
        std::string long_addr(600, 'A');
        auto r = addr::InputValidator::validateAddress(long_addr);
        if (r.valid) throw std::runtime_error("600-char should be invalid");
        if (r.error.find("too long") == std::string::npos)
            throw std::runtime_error("Error should mention 'too long': " + r.error);
    });

    registerTest("Validator::exactlyMaxLength", []() {
        std::string addr(500, 'A');
        auto r = addr::InputValidator::validateAddress(addr);
        if (!r.valid) throw std::runtime_error("Exactly 500 chars should be valid");
    });

    registerTest("Validator::nullByte", []() {
        std::string addr = "123 Main";
        addr += '\0';
        addr += " Street";
        auto r = addr::InputValidator::validateAddress(addr);
        if (r.valid) throw std::runtime_error("Null byte should be rejected");
    });

    registerTest("Validator::controlChars", []() {
        std::string addr = "123 Main\x01 Street";
        auto r = addr::InputValidator::validateAddress(addr);
        if (r.valid) throw std::runtime_error("Control char should be rejected");
    });

    registerTest("Validator::tabsAndNewlinesAllowed", []() {
        auto r = addr::InputValidator::validateAddress("123 Main Street\tNew York\nNY 10001");
        if (!r.valid) throw std::runtime_error("Tabs and newlines should be allowed");
    });

    registerTest("Validator::validUTF8_hindi", []() {
        // "नई दिल्ली" in UTF-8
        auto r = addr::InputValidator::validateAddress("123 \xe0\xa4\xa8\xe0\xa4\x88 \xe0\xa4\xa6\xe0\xa4\xbf\xe0\xa4\xb2\xe0\xa5\x8d\xe0\xa4\xb2\xe0\xa5\x80 110001");
        if (!r.valid) throw std::runtime_error("Hindi UTF-8 should be valid");
    });

    registerTest("Validator::invalidUTF8", []() {
        std::string addr = "123 Main \x80\x81\x82 Street"; // invalid UTF-8 bytes
        auto r = addr::InputValidator::validateAddress(addr);
        if (r.valid) throw std::runtime_error("Invalid UTF-8 should be rejected");
    });

    registerTest("Validator::overlongUTF8", []() {
        // Overlong encoding of '/' (should be 0x2F, not C0 AF)
        std::string addr = "123 Main \xc0\xaf Street";
        auto r = addr::InputValidator::validateAddress(addr);
        if (r.valid) throw std::runtime_error("Overlong UTF-8 should be rejected");
    });

    registerTest("Validator::sanitize_controlChars", []() {
        std::string input = "123\x01\x02 Main\tStreet\n10001";
        std::string result = addr::InputValidator::sanitize(input);
        // Control chars stripped, tab/newline become space
        if (result.find('\x01') != std::string::npos)
            throw std::runtime_error("Control char not stripped");
        if (result.find('\t') != std::string::npos)
            throw std::runtime_error("Tab not converted to space");
    });

    registerTest("Validator::sanitize_DEL", []() {
        std::string input = "123 Main\x7F Street";
        std::string result = addr::InputValidator::sanitize(input);
        if (result.find('\x7F') != std::string::npos)
            throw std::runtime_error("DEL char not stripped");
    });

    registerTest("Validator::sanitize_preservesNormal", []() {
        std::string input = "123 MG Road, Bengaluru, Karnataka 560001";
        std::string result = addr::InputValidator::sanitize(input);
        if (result != input)
            throw std::runtime_error("Normal input should not change: " + result);
    });

    registerTest("Validator::batchValidation_valid", []() {
        Json::Value j;
        j["addresses"] = Json::arrayValue;
        j["addresses"].append("addr1");
        j["addresses"].append("addr2");
        auto r = addr::InputValidator::validateBatch(j, 2000);
        if (!r.valid) throw std::runtime_error("Valid batch rejected: " + r.error);
    });

    registerTest("Validator::batchValidation_empty", []() {
        Json::Value j;
        j["addresses"] = Json::arrayValue;
        auto r = addr::InputValidator::validateBatch(j, 2000);
        if (r.valid) throw std::runtime_error("Empty batch should be invalid");
    });

    registerTest("Validator::batchValidation_tooLarge", []() {
        Json::Value j;
        j["addresses"] = Json::arrayValue;
        for (int i = 0; i < 100; ++i) j["addresses"].append("addr" + std::to_string(i));
        auto r = addr::InputValidator::validateBatch(j, 50);
        if (r.valid) throw std::runtime_error("Batch over limit should be invalid");
    });

    registerTest("Validator::batchValidation_missingField", []() {
        Json::Value j;
        j["data"] = "something";
        auto r = addr::InputValidator::validateBatch(j, 2000);
        if (r.valid) throw std::runtime_error("Missing addresses field should be invalid");
    });
}
