#include "services/RuleEngine.h"
#include <iostream>
#include <stdexcept>
#include <functional>

extern void registerTest(const std::string& name, std::function<void()> fn);

#define ASSERT_EQ_STR(a, b) \
    if ((a) != (b)) { \
        throw std::runtime_error(std::string("Expected: '") + (b) + "' Got: '" + (a) + "'"); \
    }

void register_rule_engine_tests() {
    static addr::RuleEngine rules;

    registerTest("RuleEngine::inferState_Delhi", []() {
        addr::ParsedAddress p;
        p.postcode = "110001";
        rules.apply(p);
        ASSERT_EQ_STR(p.state, "delhi");
    });

    registerTest("RuleEngine::inferState_Karnataka", []() {
        addr::ParsedAddress p;
        p.postcode = "560001";
        rules.apply(p);
        ASSERT_EQ_STR(p.state, "karnataka");
    });

    registerTest("RuleEngine::inferState_Maharashtra", []() {
        addr::ParsedAddress p;
        p.postcode = "400001";
        rules.apply(p);
        ASSERT_EQ_STR(p.state, "maharashtra");
    });

    registerTest("RuleEngine::inferState_TamilNadu", []() {
        addr::ParsedAddress p;
        p.postcode = "600001";
        rules.apply(p);
        ASSERT_EQ_STR(p.state, "tamil nadu");
    });

    registerTest("RuleEngine::inferCity_Delhi", []() {
        addr::ParsedAddress p;
        p.postcode = "110001";
        rules.apply(p);
        ASSERT_EQ_STR(p.city, "new delhi");
    });

    registerTest("RuleEngine::inferCity_Mumbai", []() {
        addr::ParsedAddress p;
        p.postcode = "400001";
        rules.apply(p);
        ASSERT_EQ_STR(p.city, "mumbai");
    });

    registerTest("RuleEngine::inferCity_Bengaluru", []() {
        addr::ParsedAddress p;
        p.postcode = "560001";
        rules.apply(p);
        ASSERT_EQ_STR(p.city, "bengaluru");
    });

    registerTest("RuleEngine::noOverwriteExistingState", []() {
        addr::ParsedAddress p;
        p.postcode = "110001";
        p.state = "existing state";
        rules.apply(p);
        // Should NOT overwrite existing state
        ASSERT_EQ_STR(p.state, "existing state");
    });

    registerTest("RuleEngine::inferCountry_IndianPIN", []() {
        addr::ParsedAddress p;
        p.postcode = "560001";
        rules.apply(p);
        ASSERT_EQ_STR(p.country, "india");
    });

    registerTest("RuleEngine::inferCountry_USZIP", []() {
        addr::ParsedAddress p;
        p.postcode = "10001";
        p.state = "NY";
        rules.apply(p);
        ASSERT_EQ_STR(p.country, "united states");
    });

    registerTest("RuleEngine::fixMisspelling_Bangalore", []() {
        addr::ParsedAddress p;
        p.city = "bangalore";
        rules.apply(p);
        ASSERT_EQ_STR(p.city, "bengaluru");
    });

    registerTest("RuleEngine::fixMisspelling_Bombay", []() {
        addr::ParsedAddress p;
        p.city = "bombay";
        rules.apply(p);
        ASSERT_EQ_STR(p.city, "mumbai");
    });

    registerTest("RuleEngine::fixMisspelling_Calcutta", []() {
        addr::ParsedAddress p;
        p.city = "calcutta";
        rules.apply(p);
        ASSERT_EQ_STR(p.city, "kolkata");
    });

    registerTest("RuleEngine::fixMisspelling_Gurgaon", []() {
        addr::ParsedAddress p;
        p.city = "gurgaon";
        rules.apply(p);
        ASSERT_EQ_STR(p.city, "gurugram");
    });

    registerTest("RuleEngine::normalizeState_ka", []() {
        addr::ParsedAddress p;
        p.state = "ka";
        rules.apply(p);
        ASSERT_EQ_STR(p.state, "karnataka");
    });

    registerTest("RuleEngine::normalizeState_orissa", []() {
        addr::ParsedAddress p;
        p.state = "orissa";
        rules.apply(p);
        ASSERT_EQ_STR(p.state, "odisha");
    });

    registerTest("RuleEngine::noChangeOnUnknownPIN", []() {
        addr::ParsedAddress p;
        p.postcode = "999999"; // Invalid PIN prefix
        bool modified = rules.apply(p);
        // Country should not be inferred (9 is not a valid first digit)
        ASSERT_EQ_STR(p.state, "");
    });

    registerTest("RuleEngine::fullPipeline_IndianAddress", []() {
        addr::ParsedAddress p;
        p.postcode = "110001";
        p.city = "bangalore";  // misspelled + wrong city for this PIN
        rules.apply(p);

        // City misspelling should be fixed
        ASSERT_EQ_STR(p.city, "bengaluru");
        // State should be inferred from PIN
        ASSERT_EQ_STR(p.state, "delhi");
        // Country should be inferred
        ASSERT_EQ_STR(p.country, "india");
    });
}
