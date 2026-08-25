#ifndef ACPP_STL_ITERATOR_HPP
#define ACPP_STL_ITERATOR_HPP

// Module 4 -- the freestanding seam. One file per standard header we depend on.
//
// The `using` list below IS the dependency manifest: it is the complete,
// auditable inventory of what this library needs from <iterator>. Drop a
// replacement at acpp/ext/stl/iterator.hpp on the include path and __has_include
// picks it up -- no build flag, no fork, no patch to rebase.

#if __has_include(<acpp/ext/stl/iterator.hpp>)
#    include <acpp/ext/stl/iterator.hpp>
#else
#    include <iterator>

namespace acpp::stl {

using std::input_iterator_tag;
using std::forward_iterator_tag;
using std::bidirectional_iterator_tag;
using std::random_access_iterator_tag;
using std::contiguous_iterator_tag;
using std::iterator_traits;
using std::reverse_iterator;
using std::distance;
using std::advance;
using std::next;
using std::prev;
using std::begin;
using std::end;

} // namespace acpp::stl

#endif

#endif // ACPP_STL_ITERATOR_HPP
