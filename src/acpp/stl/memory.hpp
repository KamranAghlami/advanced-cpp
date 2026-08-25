#ifndef ACPP_STL_MEMORY_HPP
#define ACPP_STL_MEMORY_HPP

// Module 4 -- the freestanding seam. One file per standard header we depend on.
//
// The `using` list below IS the dependency manifest: it is the complete,
// auditable inventory of what this library needs from <memory>. Drop a
// replacement at acpp/ext/stl/memory.hpp on the include path and __has_include
// picks it up -- no build flag, no fork, no patch to rebase.

#if __has_include(<acpp/ext/stl/memory.hpp>)
#    include <acpp/ext/stl/memory.hpp>
#else
#    include <memory>

namespace acpp::stl {

using std::allocator;
using std::allocator_traits;
using std::addressof;
using std::to_address;
using std::construct_at;
using std::destroy_at;
using std::destroy;
using std::uninitialized_fill;
using std::uninitialized_fill_n;
using std::uninitialized_value_construct_n;
using std::unique_ptr;
using std::make_unique;
using std::pointer_traits;
using std::uses_allocator_construction_args;
using std::make_obj_using_allocator;

} // namespace acpp::stl

#endif

#endif // ACPP_STL_MEMORY_HPP
