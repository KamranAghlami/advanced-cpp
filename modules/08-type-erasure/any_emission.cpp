// Module 8, exercise 2 (nm half) -- "verify that only the operations you use
// get emitted".
//
// This TU instantiates basic_any for two types and uses a *different* subset of
// the operations for each. The single-function vtable means every operation for
// a type lives in one symbol, so what nm can actually tell you is which
// *types'* vtables were emitted -- and, via the symbol size, roughly how much
// of the switch survived.
//
// Checked by symbols_any_* in this directory's CMakeLists.txt. Inspect by hand:
//   nm -C --size-sort build/modules/08-type-erasure/CMakeFiles/any_emission.dir/any_emission.cpp.o

#include <cstdint>
#include <string>

#include <acpp/any.hpp>

namespace {

struct used_fully {
    int a, b;

    [[nodiscard]] bool operator==(const used_fully &) const noexcept = default;
};

struct never_instantiated {
    double values[8];
};

} // namespace

extern "C" {

// Exercises info, copy, destroy and compare for `used_fully`.
bool acpp_probe_any_roundtrip(int a, int b) {
    acpp::any value{used_fully{a, b}};
    acpp::any copy{value};
    return value == copy && value.type() == acpp::type_id<used_fully>();
}

// Only ever holds an int, and only ever asks for its type.
unsigned acpp_probe_any_type_only(int v) {
    const acpp::any value{v};
    return value.type().hash();
}

} // extern "C"

// `never_instantiated` is named in this TU but never put into an any, so no
// vtable for it may exist. If one does, something is instantiating eagerly.
static_assert(sizeof(never_instantiated) == 64u);
