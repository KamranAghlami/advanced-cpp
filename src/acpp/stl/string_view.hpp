#ifndef ACPP_STL_STRING_VIEW_HPP
#define ACPP_STL_STRING_VIEW_HPP

// Module 4 -- the freestanding seam. One file per standard header we depend on.
//
// The `using` list below IS the dependency manifest: it is the complete,
// auditable inventory of what this library needs from <string_view>. Drop a
// replacement at acpp/ext/stl/string_view.hpp on the include path and __has_include
// picks it up -- no build flag, no fork, no patch to rebase.

#if __has_include(<acpp/ext/stl/string_view.hpp>)
#    include <acpp/ext/stl/string_view.hpp>
#else
#    include <string_view>

namespace acpp::stl {

using std::string_view;
using std::basic_string_view;

} // namespace acpp::stl

#endif

#endif // ACPP_STL_STRING_VIEW_HPP
