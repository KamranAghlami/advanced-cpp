#ifndef ACPP_STL_CONCEPTS_HPP
#define ACPP_STL_CONCEPTS_HPP

// Module 4 -- the freestanding seam. One file per standard header we depend on.
//
// The `using` list below IS the dependency manifest: it is the complete,
// auditable inventory of what this library needs from <concepts>. Drop a
// replacement at acpp/ext/stl/concepts.hpp on the include path and __has_include
// picks it up -- no build flag, no fork, no patch to rebase.

#if __has_include(<acpp/ext/stl/concepts.hpp>)
#    include <acpp/ext/stl/concepts.hpp>
#else
#    include <concepts>

namespace acpp::stl {

using std::floating_point;
using std::integral;
using std::same_as;
using std::signed_integral;
using std::unsigned_integral;
using std::convertible_to;
using std::derived_from;
using std::constructible_from;
using std::default_initializable;
using std::move_constructible;
using std::copy_constructible;
using std::invocable;
using std::regular_invocable;
using std::predicate;
using std::equality_comparable;
using std::totally_ordered;

} // namespace acpp::stl

#endif

#endif // ACPP_STL_CONCEPTS_HPP
