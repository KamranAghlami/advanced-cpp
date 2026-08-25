#ifndef ACPP_STL_NEW_HPP
#define ACPP_STL_NEW_HPP

// Module 4 -- the freestanding seam. One file per standard header we depend on.
//
// The `using` list below IS the dependency manifest: it is the complete,
// auditable inventory of what this library needs from <new>. Drop a
// replacement at acpp/ext/stl/new.hpp on the include path and __has_include
// picks it up -- no build flag, no fork, no patch to rebase.

#if __has_include(<acpp/ext/stl/new.hpp>)
#    include <acpp/ext/stl/new.hpp>
#else
#    include <new>

namespace acpp::stl {

using std::launder;
using std::nothrow_t;
using std::nothrow;
using std::bad_alloc;

} // namespace acpp::stl

#endif

#endif // ACPP_STL_NEW_HPP
