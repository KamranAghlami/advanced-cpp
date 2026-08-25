#ifndef ACPP_TESTING_HPP
#define ACPP_TESTING_HPP

// The shape Module 0's smoke tests established: one `ok`/`FAIL` line per check,
// a summary line, and a non-zero exit code on failure. Deliberately not a test
// framework -- no fixtures, no discovery, no dependency to install. It has to
// stay usable as a standalone readiness check after a toolchain or pin change,
// which means it has to keep working when nothing else in the tree does.

#include <cstdio>
#include <string_view>

namespace acpp::testing {

class suite {
public:
    explicit suite(const std::string_view name) noexcept
        : label{name} {
        std::printf("%.*s\n", static_cast<int>(label.size()), label.data());
    }

    void check(const bool ok, const std::string_view what) noexcept {
        std::printf("  %s  %.*s\n", ok ? "ok  " : "FAIL", static_cast<int>(what.size()), what.data());
        failures += static_cast<int>(!ok);
        ++total;
    }

    // For the measurement exercises: recorded in the log, never pass/fail.
    template<typename... Args>
    void note(const char *fmt, Args... args) noexcept {
        std::printf("  ....  ");
        std::printf(fmt, args...);
        std::printf("\n");
    }

    [[nodiscard]] int report() noexcept {
        std::printf("%.*s: %s (%d checks, %d failed)\n",
                    static_cast<int>(label.size()), label.data(),
                    failures == 0 ? "PASS" : "FAIL", total, failures);
        return failures == 0 ? 0 : 1;
    }

private:
    std::string_view label;
    int failures{};
    int total{};
};

} // namespace acpp::testing

#endif // ACPP_TESTING_HPP
