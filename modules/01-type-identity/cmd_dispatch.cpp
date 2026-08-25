// Module 1, exercise 2 -- a constexpr string -> ID mapper.
//
//   enum class cmd : uint32_t { power_on = "POWER_ON"_hs, ... };
//
// The enumerator's *value* is the hash of its own spelling, folded at compile
// time. A switch over those enumerators is a switch over integer immediates: the
// strings never exist at runtime unless you ask for them.
//
// Why this is worth the trouble on a constrained target: the wire format and the
// log format stay human-readable, the dispatch stays integral, and there is no
// table to keep in sync with the enum -- the enum *is* the table.
//
// The codegen claim (jump table or compare chain over immediates, no strcmp) is
// checked by the asm probe next door in cmd_dispatch_codegen.cpp.

#include <array>
#include <cstdint>
#include <string_view>

#include <acpp/hashed_string.hpp>
#include <acpp/testing.hpp>

namespace {

using namespace acpp::literals;

enum class cmd : std::uint32_t {
    power_on = "POWER_ON"_hs,
    power_off = "POWER_OFF"_hs,
    reboot = "REBOOT"_hs,
    self_test = "SELF_TEST"_hs,
    read_temp = "READ_TEMP"_hs,
    unknown = 0u,
};

// ---------------------------------------------------------------------------
// The collision question, made structural.
//
// hashed_string.hpp warns that FNV-1a is not collision-free. For a dispatch
// table a collision means "the wrong command runs", which is not a cost anyone
// should absorb silently. The closed set of commands is known at compile time,
// so the check is a static_assert and the cost is zero.
//
// This is the part of the pattern people copy without copying.
// ---------------------------------------------------------------------------
inline constexpr std::array all_commands{
    cmd::power_on, cmd::power_off, cmd::reboot, cmd::self_test, cmd::read_temp};

consteval bool all_distinct() noexcept {
    for(std::size_t i = 0u; i < all_commands.size(); ++i) {
        if(all_commands[i] == cmd::unknown) {
            return false; // would alias the sentinel
        }

        for(std::size_t j = i + 1u; j < all_commands.size(); ++j) {
            if(all_commands[i] == all_commands[j]) {
                return false;
            }
        }
    }

    return true;
}

static_assert(all_distinct(), "two commands hash to the same value -- rename one");

// Parsing an incoming string is one fold; the switch is then integral.
[[nodiscard]] constexpr cmd parse(const std::string_view text) noexcept {
    switch(acpp::hashed_string::value(text.data(), text.size())) {
    case static_cast<std::uint32_t>(cmd::power_on):
        return cmd::power_on;
    case static_cast<std::uint32_t>(cmd::power_off):
        return cmd::power_off;
    case static_cast<std::uint32_t>(cmd::reboot):
        return cmd::reboot;
    case static_cast<std::uint32_t>(cmd::self_test):
        return cmd::self_test;
    case static_cast<std::uint32_t>(cmd::read_temp):
        return cmd::read_temp;
    default:
        return cmd::unknown;
    }
}

[[nodiscard]] constexpr int execute(const cmd command) noexcept {
    switch(command) {
    case cmd::power_on:
        return 1;
    case cmd::power_off:
        return 2;
    case cmd::reboot:
        return 3;
    case cmd::self_test:
        return 4;
    case cmd::read_temp:
        return 5;
    case cmd::unknown:
        break;
    }

    return -1;
}

// The entire pipeline is usable in a constant expression, which means a command
// known at compile time costs nothing at all.
static_assert(execute(parse("REBOOT")) == 3);
static_assert(execute(parse("NOPE")) == -1);
static_assert(parse("POWER_ON") == cmd::power_on);

} // namespace

int main() {
    acpp::testing::suite suite{"module 01 / cmd_dispatch"};

    suite.check(execute(parse("POWER_ON")) == 1, "POWER_ON dispatches");
    suite.check(execute(parse("READ_TEMP")) == 5, "READ_TEMP dispatches");
    suite.check(execute(parse("power_on")) == -1, "hashing is case sensitive");
    suite.check(execute(parse("")) == -1, "empty input is rejected");
    suite.check(parse("POWER_ON") != parse("POWER_OFF"), "sibling commands are distinct");

    // Runtime and compile-time paths must agree -- same fold, same value.
    suite.check(acpp::hashed_string::value("REBOOT", 6u) == static_cast<std::uint32_t>(cmd::reboot),
                "runtime fold matches the compile-time enumerator");

    // A truncated buffer must not silently match: the length is part of the fold.
    suite.check(parse(std::string_view{"POWER_ONX", 8u}) == cmd::power_on, "length-delimited view hashes correctly");
    suite.check(parse(std::string_view{"POWER_ONX", 9u}) == cmd::unknown, "one extra byte changes the hash");

    suite.note("cmd::power_on = 0x%08x", static_cast<unsigned>(cmd::power_on));

    return suite.report();
}
