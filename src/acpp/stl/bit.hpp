#ifndef ACPP_STL_BIT_HPP
#define ACPP_STL_BIT_HPP

// Module 4 -- the freestanding seam. One file per standard header we depend on.
//
// The `using` list below IS the dependency manifest: it is the complete,
// auditable inventory of what this library needs from <bit>. Drop a
// replacement at acpp/ext/stl/bit.hpp on the include path and __has_include
// picks it up -- no build flag, no fork, no patch to rebase.

#if __has_include(<acpp/ext/stl/bit.hpp>)
#    include <acpp/ext/stl/bit.hpp>
#else
#    include <bit>

namespace acpp::stl {

using std::popcount;
using std::has_single_bit;
using std::bit_ceil;
using std::bit_width;
using std::countr_zero;
using std::countl_zero;
using std::bit_cast;

} // namespace acpp::stl

#endif

#endif // ACPP_STL_BIT_HPP
