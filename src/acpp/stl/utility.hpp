#ifndef ACPP_STL_UTILITY_HPP
#define ACPP_STL_UTILITY_HPP

// Module 4 -- the freestanding seam. One file per standard header we depend on.
//
// The `using` list below IS the dependency manifest: it is the complete,
// auditable inventory of what this library needs from <utility>. Drop a
// replacement at acpp/ext/stl/utility.hpp on the include path and __has_include
// picks it up -- no build flag, no fork, no patch to rebase.

#if __has_include(<acpp/ext/stl/utility.hpp>)
#    include <acpp/ext/stl/utility.hpp>
#else
#    include <utility>

namespace acpp::stl {

using std::forward;
using std::move;
using std::move_if_noexcept;
using std::swap;
using std::exchange;
using std::declval;
using std::as_const;
using std::index_sequence;
using std::make_index_sequence;
using std::index_sequence_for;
using std::integer_sequence;
using std::in_place_t;
using std::in_place;
using std::in_place_type_t;
using std::in_place_type;
using std::piecewise_construct_t;
using std::piecewise_construct;
using std::pair;

} // namespace acpp::stl

#endif

#endif // ACPP_STL_UTILITY_HPP
