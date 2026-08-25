// Module 2, exercise 2 (cost half) -- one implementation, one list, per build.
//
// Compiled once per variant by this directory's CMakeLists.txt:
//   -DACPP_UNIQUE_IMPL=recursive | folded | entt
//   -DACPP_UNIQUE_N=<list length>
//
// Two things are measured against it (see NOTES.md):
//   * instantiation DEPTH, by binary-searching the smallest -ftemplate-depth
//     that still compiles. GCC has no -ftime-trace, so this is the substitute --
//     and it is a more direct answer to "how deep?" than reading a flame graph.
//   * wall-clock compile time, repeated, on an otherwise idle machine.
//
// There is nothing to run: the TU exists to be compiled.

#include <cstddef>
#include <type_traits>
#include <utility>

#ifndef ACPP_UNIQUE_N
#    define ACPP_UNIQUE_N 64
#endif

#if defined ACPP_UNIQUE_ENTT
#    include <entt/core/type_traits.hpp>
#else
#    include <acpp/type_traits.hpp>
#endif

namespace {

// N distinct types, generated rather than typed out.
template<std::size_t>
struct tag {};

// Each type appears twice, so the input is 2N long and the answer is N. A list
// with no duplicates would let an implementation skip the interesting branch.
template<typename>
struct build;

template<std::size_t... Index>
struct build<std::index_sequence<Index...>> {
#if defined ACPP_UNIQUE_ENTT
    using list = entt::type_list<tag<Index>..., tag<Index>...>;
    using unique = entt::type_list_unique_t<list>;
#elif defined ACPP_UNIQUE_RECURSIVE
    using list = acpp::type_list<tag<Index>..., tag<Index>...>;
    using unique = acpp::type_list_unique_recursive_t<list>;
#else
    using list = acpp::type_list<tag<Index>..., tag<Index>...>;
    using unique = acpp::type_list_unique_folded_t<list>;
#endif
};

using result = build<std::make_index_sequence<ACPP_UNIQUE_N>>::unique;

static_assert(result::size == ACPP_UNIQUE_N, "the probe must actually do the work");

} // namespace
