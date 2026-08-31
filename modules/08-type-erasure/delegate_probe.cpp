// Module 8, exercise 3 -- does a delegate call inline, and where does the
// advantage actually live?
//
// The first draft of this file asserted "delegate inlines, std::function does
// not". At -O2, gcc 13.3 inlined BOTH: a std::function that is a local constant
// with a compile-time-known target gets devirtualised just as thoroughly. That
// result is kept below rather than quietly deleted, because it changes the
// recommendation -- the delegate's edge is not inlining a local, it is size, no
// allocation, trivial copyability, and what happens once the callable has to
// cross a function boundary.
//
// Checked by codegen_delegate_* and codegen_std_function_* in this directory's
// CMakeLists.txt.

#include <functional>

#include <acpp/delegate.hpp>

namespace {

struct accumulator {
    int total{};

    void add(int value) noexcept { total += value; }
};

int triple(int value) noexcept {
    return value * 3;
}

} // namespace

extern "C" {

// --- locals: both mechanisms disappear -------------------------------------

int acpp_probe_delegate_inlined(int value) {
    const acpp::delegate<int(int)> call{acpp::connect_arg<&triple>};
    return call(value);
}

// The one that made the point, and the one whose result depends on the
// compiler. Two separate things happen here, and they are asserted separately:
// the DISPATCH devirtualises (every toolchain measured, checked always), and the
// OBJECT is then deleted outright (gcc >= 13 / clang, checked there only). gcc
// 11.4 does the first and not the second -- it still calls _M_manager to destroy
// an _Any_data it proved dead. The delegate needs neither optimisation, because
// it is trivially destructible; see NOTES.md.
int acpp_probe_std_function_local(int value) {
    const std::function<int(int)> call{&triple};
    return call(value);
}

// A member function bound to an instance. The delegate's own indirection goes
// away; whether the target itself inlines is a separate question the optimiser
// answers on its own terms.
int acpp_probe_delegate_member(int value) {
    accumulator target;
    const acpp::delegate<void(int)> call{acpp::connect_arg<&accumulator::add>, target};
    call(value);
    call(value);
    return target.total;
}

// --- stored: where the difference actually is ------------------------------
//
// Once the callable arrives as a parameter, neither can be devirtualised. Now
// compare what the indirection costs.

// Two pointers in registers, one indirect call. That is the floor.
int acpp_probe_delegate_stored(const acpp::delegate<int(int)> &call, int value) {
    return call(value);
}

// std::function must check for an empty target (it is required to throw
// std::bad_function_call) and then dispatch through its manager. Strictly more
// work than the delegate, and the extra is not optional -- it is the interface.
int acpp_probe_std_function_stored(const std::function<int(int)> &call, int value) {
    return call(value);
}

} // extern "C"
