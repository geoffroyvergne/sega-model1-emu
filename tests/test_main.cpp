// Test runner: runs every registered TEST_CASE and prints a checklist.
//
// Usage: emulator_tests [--verbose] [filter]
//   filter     run only tests whose name contains this text
//   --verbose  print the emulator log of every test, not just failing ones

#include "test_framework.hpp"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>

namespace model1_test {

namespace {

std::ostringstream* g_log = nullptr;

} // namespace

std::vector<TestCase>& registry()
{
    static std::vector<TestCase> tests;
    return tests;
}

std::vector<std::string>& current_failures()
{
    static std::vector<std::string> failures;
    return failures;
}

std::string captured_log()
{
    return g_log != nullptr ? g_log->str() : std::string{};
}

} // namespace model1_test

int main(int argc, char* argv[])
{
    using namespace model1_test;

    bool verbose = false;
    const char* filter = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--verbose") == 0) {
            verbose = true;
        } else {
            filter = argv[i];
        }
    }

    std::size_t passed = 0;
    std::size_t failed = 0;
    std::cout << "Running emulator tests\n\n";

    for (const TestCase& test : registry()) {
        if (filter != nullptr && std::strstr(test.name, filter) == nullptr) {
            continue;
        }

        // Capture the emulator's diagnostics while the test runs.
        std::ostringstream log;
        g_log = &log;
        std::streambuf* const original = std::cerr.rdbuf(log.rdbuf());
        current_failures().clear();

        test.function();

        std::cerr.rdbuf(original);
        g_log = nullptr;

        const bool ok = current_failures().empty();
        std::cout << (ok ? "  [PASS] " : "  [FAIL] ") << test.name << '\n';
        for (const std::string& failure : current_failures()) {
            std::cout << "         " << failure << '\n';
        }
        if ((!ok || verbose) && !log.str().empty()) {
            std::cout << "         --- emulator log ---\n";
            std::istringstream lines(log.str());
            for (std::string line; std::getline(lines, line);) {
                std::cout << "         " << line << '\n';
            }
        }
        (ok ? passed : failed)++;
    }

    std::cout << '\n' << passed << " passed, " << failed << " failed";
    if (passed + failed == 0) {
        std::cout << " (no test matched the filter)";
    }
    std::cout << '\n';
    return failed == 0 && passed > 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
