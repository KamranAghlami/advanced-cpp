#ifndef ACPP_STL_VECTOR_HPP
#define ACPP_STL_VECTOR_HPP

// Module 4 -- the freestanding seam. One file per standard header we depend on.
//
// The `using` list below IS the dependency manifest: it is the complete,
// auditable inventory of what this library needs from <vector>. Drop a
// replacement at acpp/ext/stl/vector.hpp on the include path and __has_include
// picks it up -- no build flag, no fork, no patch to rebase.

#if __has_include(<acpp/ext/stl/vector.hpp>)
#    include <acpp/ext/stl/vector.hpp>
#else
#    include <vector>

namespace acpp::stl {

using std::vector;

} // namespace acpp::stl

#endif

#endif // ACPP_STL_VECTOR_HPP
