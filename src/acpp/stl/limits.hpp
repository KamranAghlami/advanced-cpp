#ifndef ACPP_STL_LIMITS_HPP
#define ACPP_STL_LIMITS_HPP

// Module 4 -- the freestanding seam. One file per standard header we depend on.
//
// The `using` list below IS the dependency manifest: it is the complete,
// auditable inventory of what this library needs from <limits>. Drop a
// replacement at acpp/ext/stl/limits.hpp on the include path and __has_include
// picks it up -- no build flag, no fork, no patch to rebase.

#if __has_include(<acpp/ext/stl/limits.hpp>)
#    include <acpp/ext/stl/limits.hpp>
#else
#    include <limits>

namespace acpp::stl {

using std::numeric_limits;

} // namespace acpp::stl

#endif

#endif // ACPP_STL_LIMITS_HPP
