#ifndef ACPP_STL_CSTDDEF_HPP
#define ACPP_STL_CSTDDEF_HPP

// Module 4 -- the freestanding seam. One file per standard header we depend on.
//
// The `using` list below IS the dependency manifest: it is the complete,
// auditable inventory of what this library needs from <cstddef>. Drop a
// replacement at acpp/ext/stl/cstddef.hpp on the include path and __has_include
// picks it up -- no build flag, no fork, no patch to rebase.

#if __has_include(<acpp/ext/stl/cstddef.hpp>)
#    include <acpp/ext/stl/cstddef.hpp>
#else
#    include <cstddef>

namespace acpp::stl {

using std::size_t;
using std::ptrdiff_t;
using std::byte;
using std::nullptr_t;

} // namespace acpp::stl

#endif

#endif // ACPP_STL_CSTDDEF_HPP
