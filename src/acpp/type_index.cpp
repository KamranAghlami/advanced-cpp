// Module 1.3 -- the one and only definition of the sequential-ID counter.
//
// This file exists so there is exactly one counter object in the process image.
// A header-only `static id_type value{}` inside next() would be instantiated in
// every TU that included it -- fine within one binary, because the linker folds
// inline definitions, but NOT across shared objects unless the symbol is
// visible. Hence ACPP_API on the declaration in type_info.hpp.
//
// ACPP_MAYBE_ATOMIC covers the other half: two threads asking for the ID of two
// different types race on the increment. The static-local guard serialises each
// *initialiser*, not the counter they share.

#include "type_info.hpp"

namespace acpp::internal {

id_type type_index::next() noexcept {
    static ACPP_MAYBE_ATOMIC(id_type) value{};
    return value++;
}

} // namespace acpp::internal
