// Module 1.3 -- the one and only definition of the sequential-ID counter.
//
// This file exists so there is exactly one counter object in the process image.
// A header-only `static id_type value{}` inside next() would be instantiated in
// every TU that included it -- fine within one binary, because the linker folds
// inline definitions, but NOT across shared objects unless the symbol is
// visible. Hence ACPP_API on the declaration in type_info.hpp.
//
// The atomicity of the counter is a policy, not a constant: two threads asking
// for the IDs of two different types race on the increment, but a single-threaded
// freestanding build should not pay for that. Module 2 moved the decision into
// counter_traits, so it is spelled here as a tag rather than a macro.

#include "counter.hpp"
#include "type_info.hpp"

namespace acpp::internal {

id_type type_index::next() noexcept {
    return sequential_counter<type_index>::next();
}

} // namespace acpp::internal
