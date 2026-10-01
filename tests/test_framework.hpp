#pragma once

// Minimal, dependency-free unit test framework for the emulator.
//
//   TEST_CASE(name) { ... }         defines and registers a test
//   CHECK(expression)               records a failure if false, test continues
//   CHECK_EQ(actual, expected)      same, and prints both values in hex
//
// The runner (test_main.cpp) captures everything the emulator writes to
// std::cerr while a test runs. The capture is shown only for failing tests,
// and tests can inspect it with model1_test::captured_log().

#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

namespace model1_test {

struct TestCase {
    const char* name;
    void (*function)();
};

std::vector<TestCase>& registry();

struct Registrar {
    Registrar(const char* name, void (*function)()) { registry().push_back({name, function}); }
};

// Failure messages recorded by CHECK / CHECK_EQ for the running test.
std::vector<std::string>& current_failures();

// Emulator log (std::cerr) captured so far during the running test.
std::string captured_log();

inline void check(bool condition, const char* expression, const char* file, int line)
{
    if (!condition) {
        std::ostringstream message;
        message << file << ':' << line << ": CHECK(" << expression << ") failed";
        current_failures().push_back(message.str());
    }
}

template <typename A, typename B>
void check_eq(const A& actual, const B& expected, const char* actual_text, const char* expected_text,
              const char* file, int line)
{
    if (!(actual == expected)) {
        std::ostringstream message;
        message << file << ':' << line << ": CHECK_EQ(" << actual_text << ", " << expected_text << ") failed: got 0x"
                << std::hex << std::uppercase << static_cast<uint64_t>(actual) << ", expected 0x"
                << static_cast<uint64_t>(expected);
        current_failures().push_back(message.str());
    }
}

} // namespace model1_test

#define MODEL1_TEST_CONCAT_INNER(a, b) a##b
#define MODEL1_TEST_CONCAT(a, b) MODEL1_TEST_CONCAT_INNER(a, b)

#define TEST_CASE(name)                                                                     \
    static void name();                                                                     \
    static const ::model1_test::Registrar MODEL1_TEST_CONCAT(name, _registrar)(#name, &name); \
    static void name()

#define CHECK(expression) ::model1_test::check(static_cast<bool>(expression), #expression, __FILE__, __LINE__)

#define CHECK_EQ(actual, expected) \
    ::model1_test::check_eq((actual), (expected), #actual, #expected, __FILE__, __LINE__)
