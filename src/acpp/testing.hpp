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
        // Unbuffered, deliberately. A test that hangs must still show how far
        // it got -- with the default block buffering on a pipe, a hung test
        // reports absolutely nothing, which is the least useful failure mode a
        // test harness has. Phase C made this non-negotiable.
        std::setvbuf(stdout, nullptr, _IONBF, 0);
        std::printf("%.*s\n", static_cast<int>(label.size()), label.data());
    }

    void check(const bool ok, const std::string_view what) noexcept {
        std::printf("  %s  %.*s\n", ok ? "ok  " : "FAIL", static_cast<int>(what.size()), what.data());
        failures += static_cast<int>(!ok);
        ++total;
    }

    // For the measurement exercises: recorded in the log, never pass/fail.
    //
    // Two overloads, because one is not enough. `printf(fmt)` with a non-literal
    // format and no arguments is what -Wformat-security exists to catch: if
    // `fmt` ever came from anywhere but a literal, a stray %s reads whatever is
    // in the argument registers. The no-argument case therefore goes through
    // "%s" and never treats its input as a format at all.
    void note(const char *text) noexcept {
        std::printf("  ....  %s\n", text);
    }

    template<typename Arg, typename... Args>
    void note(const char *fmt, Arg first, Args... rest) noexcept {
        std::printf("  ....  ");
        // NOLINTNEXTLINE(cert-err33-c) -- diagnostics; a short write is not
        // worth handling in a test harness.
        std::printf(fmt, first, rest...);
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
