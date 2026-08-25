#ifndef ACPP_STL_OPTIONAL_HPP
#define ACPP_STL_OPTIONAL_HPP

// Module 4 -- the freestanding seam. One file per standard header we depend on.
//
// The `using` list below IS the dependency manifest: it is the complete,
// auditable inventory of what this library needs from <optional>. Drop a
// replacement at acpp/ext/stl/optional.hpp on the include path and __has_include
// picks it up -- no build flag, no fork, no patch to rebase.

#if __has_include(<acpp/ext/stl/optional.hpp>)
#    include <acpp/ext/stl/optional.hpp>
#else
#    include <optional>

namespace acpp::stl {

using std::optional;
using std::nullopt;
using std::nullopt_t;
using std::make_optional;

} // namespace acpp::stl

#endif

#endif // ACPP_STL_OPTIONAL_HPP
