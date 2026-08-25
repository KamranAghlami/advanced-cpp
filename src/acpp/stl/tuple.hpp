#ifndef ACPP_STL_TUPLE_HPP
#define ACPP_STL_TUPLE_HPP

// Module 4 -- the freestanding seam. One file per standard header we depend on.
//
// The `using` list below IS the dependency manifest: it is the complete,
// auditable inventory of what this library needs from <tuple>. Drop a
// replacement at acpp/ext/stl/tuple.hpp on the include path and __has_include
// picks it up -- no build flag, no fork, no patch to rebase.

#if __has_include(<acpp/ext/stl/tuple.hpp>)
#    include <acpp/ext/stl/tuple.hpp>
#else
#    include <tuple>

namespace acpp::stl {

using std::tuple;
using std::tuple_cat;
using std::tuple_size;
using std::tuple_size_v;
using std::tuple_element;
using std::tuple_element_t;
using std::get;
using std::apply;
using std::make_tuple;
using std::forward_as_tuple;
using std::ignore;

} // namespace acpp::stl

#endif

#endif // ACPP_STL_TUPLE_HPP
