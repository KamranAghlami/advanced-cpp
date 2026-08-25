// Module 1, exercise 1 (codegen half).
//
// The claim under test: at -O2, asking for a type's name costs nothing at
// runtime -- the string_view points into a string the compiler emitted anyway,
// and both of its words are immediates.
//
// This TU is compiled to assembly by the acpp_asm_probe() rule in this
// directory's CMakeLists.txt and the result is pattern-checked by
// cmake/check_asm.cmake. extern "C" keeps the symbol names greppable.
//
// Per docs/CLAUDE.md's "measure, don't assert": nothing here claims a result.
// The build checks it.

#include <cstddef>
#include <vector>

#include <acpp/type_info.hpp>

extern "C" {

// The whole technique in one function. If this compiles to anything but
// "load two immediates and return", the claim is false.
const char *acpp_probe_name_data() {
    return acpp::type_name<std::vector<int>>::value().data();
}

std::size_t acpp_probe_name_size() {
    return acpp::type_name<std::vector<int>>::value().size();
}

// The hash is a fold over the name. At compile time it is one integer.
unsigned acpp_probe_hash() {
    return acpp::type_hash<std::vector<int>>::value();
}

// Contrast: the sequential ID *cannot* be compile-time -- it depends on the
// order every other type in the process was first touched. Expect a real
// function-local static here: a guard variable load and a branch. Included so
// the comparison is visible in the same asm file rather than asserted in prose.
unsigned acpp_probe_index() {
    return acpp::type_index<std::vector<int>>::value();
}

} // extern "C"
