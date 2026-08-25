#ifndef ACPP_COMPONENT_HPP
#define ACPP_COMPONENT_HPP

// Module 2 -- behaviour inferred from type properties, and the customization
// ladder that lets a user override the inference without touching the library.
//
// The ladder, cheapest rung first:
//
//   1. inferred   nothing to write; the default is derived from the type
//   2. opt in     `static constexpr bool in_place_delete = true;` in your type
//   3. override   specialize acpp::component_traits<Your, Entity>
//
// Each rung is more invasive than the last, and level 2 is the one that matters
// in practice: the user opts in from inside their own type, with no macro and no
// specialization in someone else's namespace.

#include <concepts>
#include <cstddef>
#include <type_traits>

#include "config.hpp"

namespace acpp {

namespace internal {

// ---------------------------------------------------------------------------
// Deletion policy, inferred from movability.
//
// This is a CORRECTNESS default, not a performance one. swap-and-pop deletion
// moves the last element into the hole; a type that cannot be move-constructed
// and move-assigned cannot be swap-and-popped at all. So the inference is not
// "non-movable types are probably pointer-sensitive" -- it is "swap-and-pop is
// not an available implementation for this type". See NOTES.md.
// ---------------------------------------------------------------------------
template<typename Type>
struct in_place_delete
    : std::bool_constant<!(std::is_move_constructible_v<Type> && std::is_move_assignable_v<Type>)> {};

template<>
struct in_place_delete<void>: std::false_type {};

// Level 2. A constrained partial specialization, which in C++17 would have been
// void_t detection plus two layers of conditional_t.
template<typename Type>
    requires Type::in_place_delete
struct in_place_delete<Type>: std::true_type {};

// ---------------------------------------------------------------------------
// Page size, inferred from emptiness.
//
// The `!is_empty_v<...> *` is the idiom worth stealing: a branch collapsed into
// arithmetic at compile time. An empty type gets page_size 0, which downstream
// means "allocate no payload at all" -- a tag component costs one entity id.
// ---------------------------------------------------------------------------
template<typename Type>
struct page_size
    : std::integral_constant<std::size_t, !std::is_empty_v<ACPP_ETO_TYPE(Type)> * ACPP_PACKED_PAGE> {};

template<>
struct page_size<void>: std::integral_constant<std::size_t, 0u> {};

template<typename Type>
    requires std::convertible_to<decltype(Type::page_size), std::size_t>
struct page_size<Type>: std::integral_constant<std::size_t, Type::page_size> {};

} // namespace internal

template<typename Type>
concept cvref_unqualified = std::same_as<Type, std::remove_cvref_t<Type>>;

/**
 * Level 3: specialize this to override everything at once. Kept deliberately
 * small -- every member here is a decision a storage has to make, and nothing
 * else belongs in it.
 */
template<cvref_unqualified Type, typename Entity>
struct component_traits {
    using element_type = Type;
    using entity_type = Entity;

    static constexpr bool in_place_delete = internal::in_place_delete<Type>::value;
    static constexpr std::size_t page_size = internal::page_size<Type>::value;
};

} // namespace acpp

#endif // ACPP_COMPONENT_HPP
