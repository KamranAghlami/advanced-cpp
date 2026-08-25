#ifndef ACPP_STL_FUNCTIONAL_HPP
#define ACPP_STL_FUNCTIONAL_HPP

// Module 4 -- the freestanding seam. One file per standard header we depend on.
//
// The `using` list below IS the dependency manifest: it is the complete,
// auditable inventory of what this library needs from <functional>. Drop a
// replacement at acpp/ext/stl/functional.hpp on the include path and __has_include
// picks it up -- no build flag, no fork, no patch to rebase.

#if __has_include(<acpp/ext/stl/functional.hpp>)
#    include <acpp/ext/stl/functional.hpp>
#else
#    include <functional>

namespace acpp::stl {

using std::invoke;
using std::reference_wrapper;
using std::ref;
using std::cref;
using std::less;
using std::equal_to;
using std::hash;

} // namespace acpp::stl

#endif

#endif // ACPP_STL_FUNCTIONAL_HPP
