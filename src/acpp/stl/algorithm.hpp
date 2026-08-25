#ifndef ACPP_STL_ALGORITHM_HPP
#define ACPP_STL_ALGORITHM_HPP

// Module 4 -- the freestanding seam. One file per standard header we depend on.
//
// The `using` list below IS the dependency manifest: it is the complete,
// auditable inventory of what this library needs from <algorithm>. Drop a
// replacement at acpp/ext/stl/algorithm.hpp on the include path and __has_include
// picks it up -- no build flag, no fork, no patch to rebase.

#if __has_include(<acpp/ext/stl/algorithm.hpp>)
#    include <acpp/ext/stl/algorithm.hpp>
#else
#    include <algorithm>

namespace acpp::stl {

using std::min;
using std::max;
using std::swap_ranges;
using std::copy;
using std::move_backward;
using std::fill_n;
using std::find_if;
using std::all_of;
using std::any_of;
using std::none_of;
using std::remove;
using std::sort;
using std::rotate;

} // namespace acpp::stl

#endif

#endif // ACPP_STL_ALGORITHM_HPP
