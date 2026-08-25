#ifndef ACPP_STL_ARRAY_HPP
#define ACPP_STL_ARRAY_HPP

// Module 4 -- the freestanding seam. One file per standard header we depend on.
//
// The `using` list below IS the dependency manifest: it is the complete,
// auditable inventory of what this library needs from <array>. Drop a
// replacement at acpp/ext/stl/array.hpp on the include path and __has_include
// picks it up -- no build flag, no fork, no patch to rebase.

#if __has_include(<acpp/ext/stl/array.hpp>)
#    include <acpp/ext/stl/array.hpp>
#else
#    include <array>

namespace acpp::stl {

using std::array;

} // namespace acpp::stl

#endif

#endif // ACPP_STL_ARRAY_HPP
