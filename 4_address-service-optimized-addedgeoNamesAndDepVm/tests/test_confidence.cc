#include "services/ConfidenceScorer.h"
#include <iostream>
#include <stdexcept>
#include <functional>

extern void registerTest(const std::string& name, std::function<void()> fn);

void register_confidence_tests() {
    static addr::ConfidenceScorer scorer;

    registerTest("Confidence::fullAddress_highScore", []() {
        addr::ParsedAddress p;
        p.raw_input = "123 Main Street, New York, NY 10001, United States";
        p.house_number = "123";
        p.road = "main street";
        p.city = "new york";
        p.state = "ny";
        p.postcode = "10001";
        p.country = "united states";

        double score = scorer.score(p);
        if (score < 0.8) {
            throw std::runtime_error("Expected high score for full address, got: "
                                     + std::to_string(score));
        }
    });

    registerTest("Confidence::partialAddress_mediumScore", []() {
        addr::ParsedAddress p;
        p.raw_input = "Main Street, New York";
        p.road = "main street";
        p.city = "new york";

        double score = scorer.score(p);
        if (score > 0.85) {
            throw std::runtime_error("Expected medium score for partial address, got: "
                                     + std::to_string(score));
        }
        if (score < 0.2) {
            throw std::runtime_error("Score too low for partial address: "
                                     + std::to_string(score));
        }
    });

    registerTest("Confidence::emptyAddress_lowScore", []() {
        addr::ParsedAddress p;
        p.raw_input = "something";

        double score = scorer.score(p);
        if (score > 0.5) {
            throw std::runtime_error("Expected low score for empty parsed address, got: "
                                     + std::to_string(score));
        }
    });

    registerTest("Confidence::indianPIN_validFormat", []() {
        addr::ParsedAddress p;
        p.raw_input = "123 MG Road, Bengaluru, Karnataka 560001";
        p.house_number = "123";
        p.road = "mg road";
        p.city = "bengaluru";
        p.state = "karnataka";
        p.postcode = "560001";

        auto bd = scorer.scoreDetailed(p);
        if (bd.postcode_validity < 0.9) {
            throw std::runtime_error("Indian PIN should score high on validity: "
                                     + std::to_string(bd.postcode_validity));
        }
    });

    registerTest("Confidence::usZIP_validFormat", []() {
        addr::ParsedAddress p;
        p.raw_input = "test";
        p.postcode = "10001";

        auto bd = scorer.scoreDetailed(p);
        if (bd.postcode_validity < 0.9) {
            throw std::runtime_error("US ZIP should score high: "
                                     + std::to_string(bd.postcode_validity));
        }
    });

    registerTest("Confidence::ukPostcode_validFormat", []() {
        addr::ParsedAddress p;
        p.raw_input = "test";
        p.postcode = "SW1A 1AA";

        auto bd = scorer.scoreDetailed(p);
        if (bd.postcode_validity < 0.9) {
            throw std::runtime_error("UK postcode should score high: "
                                     + std::to_string(bd.postcode_validity));
        }
    });

    registerTest("Confidence::crossField_indianPINStateMatch", []() {
        addr::ParsedAddress p;
        p.raw_input = "test";
        p.postcode = "110001";
        p.state = "delhi";

        auto bd = scorer.scoreDetailed(p);
        if (bd.cross_field < 0.9) {
            throw std::runtime_error("PIN 110001 + delhi should match: "
                                     + std::to_string(bd.cross_field));
        }
    });

    registerTest("Confidence::crossField_indianPINStateMismatch", []() {
        addr::ParsedAddress p;
        p.raw_input = "test";
        p.postcode = "110001";   // Delhi PIN
        p.state = "karnataka";   // Wrong state

        auto bd = scorer.scoreDetailed(p);
        if (bd.cross_field > 0.6) {
            throw std::runtime_error("PIN 110001 + karnataka should score low: "
                                     + std::to_string(bd.cross_field));
        }
    });

    registerTest("Confidence::scoreBreakdown_sumConsistent", []() {
        addr::ParsedAddress p;
        p.raw_input = "123 Main St, City, State 12345";
        p.house_number = "123";
        p.road = "main st";
        p.city = "city";
        p.state = "state";
        p.postcode = "12345";

        auto bd = scorer.scoreDetailed(p);
        double expected = bd.completeness * 0.35 +
                         bd.postcode_validity * 0.25 +
                         bd.cross_field * 0.20 +
                         bd.token_coverage * 0.20;

        double diff = std::abs(bd.total - expected);
        if (diff > 0.001) {
            throw std::runtime_error("Score breakdown inconsistent: total="
                                     + std::to_string(bd.total)
                                     + " expected=" + std::to_string(expected));
        }
    });
}
