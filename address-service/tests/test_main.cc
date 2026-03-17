#include <iostream>
#include <string>
#include <vector>
#include <functional>

// Minimal test framework (no external dependency needed)
struct TestResult {
    std::string name;
    bool passed;
    std::string error;
};

static std::vector<TestResult> g_results;

#define TEST(name) \
    static void test_##name(); \
    static bool _reg_##name = ([]{ \
        g_results.push_back({#name, true, ""}); \
        return true; \
    })(); \
    static void test_##name()

#define ASSERT_TRUE(expr) \
    if (!(expr)) { \
        throw std::runtime_error(std::string("ASSERT_TRUE failed: ") + #expr \
            + " at " + __FILE__ + ":" + std::to_string(__LINE__)); \
    }

#define ASSERT_FALSE(expr) \
    if ((expr)) { \
        throw std::runtime_error(std::string("ASSERT_FALSE failed: ") + #expr \
            + " at " + __FILE__ + ":" + std::to_string(__LINE__)); \
    }

#define ASSERT_EQ(a, b) \
    if ((a) != (b)) { \
        throw std::runtime_error(std::string("ASSERT_EQ failed: ") + #a + " != " + #b \
            + " at " + __FILE__ + ":" + std::to_string(__LINE__)); \
    }

#define ASSERT_GT(a, b) \
    if (!((a) > (b))) { \
        throw std::runtime_error(std::string("ASSERT_GT failed: ") + #a + " <= " + #b \
            + " at " + __FILE__ + ":" + std::to_string(__LINE__)); \
    }

#define ASSERT_LT(a, b) \
    if (!((a) < (b))) { \
        throw std::runtime_error(std::string("ASSERT_LT failed: ") + #a + " >= " + #b \
            + " at " + __FILE__ + ":" + std::to_string(__LINE__)); \
    }

// Declare test functions from other files
void register_preprocessor_tests();
void register_parser_tests();
void register_rule_engine_tests();
void register_confidence_tests();
void register_cache_tests();

// Test registry
struct TestEntry {
    std::string name;
    std::function<void()> fn;
};

static std::vector<TestEntry> g_tests;

void registerTest(const std::string& name, std::function<void()> fn) {
    g_tests.push_back({name, std::move(fn)});
}

int main() {
    // Register all test suites
    register_preprocessor_tests();
    register_rule_engine_tests();
    register_confidence_tests();
    register_cache_tests();

    // Note: parser tests require libpostal to be installed
    // Uncomment if libpostal is available:
    // register_parser_tests();

    int passed = 0;
    int failed = 0;

    std::cout << "\n====================================\n";
    std::cout << "  Address Service Test Suite\n";
    std::cout << "====================================\n\n";

    for (auto& test : g_tests) {
        try {
            test.fn();
            std::cout << "  PASS  " << test.name << "\n";
            ++passed;
        } catch (const std::exception& e) {
            std::cout << "  FAIL  " << test.name << "\n";
            std::cout << "        " << e.what() << "\n";
            ++failed;
        }
    }

    std::cout << "\n====================================\n";
    std::cout << "  Results: " << passed << " passed, " << failed << " failed\n";
    std::cout << "====================================\n\n";

    return failed > 0 ? 1 : 0;
}
