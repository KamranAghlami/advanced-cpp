#ifndef ACPP_STL_CSTDINT_HPP
#define ACPP_STL_CSTDINT_HPP

// Module 4 -- the freestanding seam. One file per standard header we depend on.
//
// The `using` list below IS the dependency manifest: it is the complete,
// auditable inventory of what this library needs from <cstdint>. Drop a
// replacement at acpp/ext/stl/cstdint.hpp on the include path and __has_include
// picks it up -- no build flag, no fork, no patch to rebase.

#if __has_include(<acpp/ext/stl/cstdint.hpp>)
#    include <acpp/ext/stl/cstdint.hpp>
#else
#    include <cstdint>

namespace acpp::stl {

using std::int8_t;
using std::int16_t;
using std::int32_t;
using std::int64_t;
using std::uint8_t;
using std::uint16_t;
using std::uint32_t;
using std::uint64_t;
using std::uintptr_t;

} // namespace acpp::stl

#endif

#endif // ACPP_STL_CSTDINT_HPP
