// Module 1, exercise 2 (codegen half): confirm the dispatch is integral.
//
// The failure mode this rules out is a switch that quietly became a chain of
// string comparisons -- which is what you get if the hash is not actually folded
// at compile time. `call strcmp` / `call memcmp` in this body means the
// technique is not doing what the module claims.
//
// Checked by cmd_dispatch_codegen_* in this directory's CMakeLists.txt.

#include <cstdint>

#include <acpp/hashed_string.hpp>

using namespace acpp::literals;

namespace {

enum class cmd : std::uint32_t {
    power_on = "POWER_ON"_hs,
    power_off = "POWER_OFF"_hs,
    reboot = "REBOOT"_hs,
    self_test = "SELF_TEST"_hs,
    read_temp = "READ_TEMP"_hs,
};

} // namespace

extern "C" {

// Input is already an ID (the normal shape once the boundary has parsed it):
// pure integer compares against immediates, no memory touched.
int acpp_probe_dispatch(std::uint32_t id) {
    switch(id) {
    case static_cast<std::uint32_t>(cmd::power_on):
        return 1;
    case static_cast<std::uint32_t>(cmd::power_off):
        return 2;
    case static_cast<std::uint32_t>(cmd::reboot):
        return 3;
    case static_cast<std::uint32_t>(cmd::self_test):
        return 4;
    case static_cast<std::uint32_t>(cmd::read_temp):
        return 5;
    default:
        return -1;
    }
}

// A command known at the call site should collapse to a constant.
int acpp_probe_dispatch_constant() {
    return acpp_probe_dispatch("REBOOT"_hs);
}

} // extern "C"
