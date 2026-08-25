#ifndef ACPP_STL_ATOMIC_HPP
#define ACPP_STL_ATOMIC_HPP

// Module 4 -- the freestanding seam. One file per standard header we depend on.
//
// The `using` list below IS the dependency manifest: it is the complete,
// auditable inventory of what this library needs from <atomic>. Drop a
// replacement at acpp/ext/stl/atomic.hpp on the include path and __has_include
// picks it up -- no build flag, no fork, no patch to rebase.

#if __has_include(<acpp/ext/stl/atomic.hpp>)
#    include <acpp/ext/stl/atomic.hpp>
#else
#    include <atomic>

namespace acpp::stl {

using std::atomic;
using std::atomic_flag;
using std::atomic_thread_fence;
using std::memory_order;

} // namespace acpp::stl

#endif

#endif // ACPP_STL_ATOMIC_HPP
