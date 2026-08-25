#ifndef ACPP_TYPE_TRAITS_HPP
#define ACPP_TYPE_TRAITS_HPP

// Module 2.3 -- compile-time type sequence algebra, plus the four traits from
// the module's vocabulary list that later modules actually consume.
//
// type_list is not a tuple. It has no storage, no constructor and no members;
// it exists only to be pattern-matched by partial specialization. That is why
// manipulating one costs instantiations and nothing else.

#include <cstddef>
#include <type_traits>

namespace acpp {

template<typename... Type>
struct type_list {
    using type = type_list;
    static constexpr std::size_t size = sizeof...(Type);
};

// --- concatenation ---------------------------------------------------------

template<typename...>
struct type_list_cat;

template<>
struct type_list_cat<> {
    using type = type_list<>;
};

template<typename... Type>
struct type_list_cat<type_list<Type...>> {
    using type = type_list<Type...>;
};

template<typename... Type, typename... Other, typename... List>
struct type_list_cat<type_list<Type...>, type_list<Other...>, List...> {
    using type = type_list_cat<type_list<Type..., Other...>, List...>::type;
};

template<typename... List>
using type_list_cat_t = type_list_cat<List...>::type;

// --- membership ------------------------------------------------------------

template<typename, typename>
struct type_list_contains;

template<typename... Type, typename Other>
struct type_list_contains<type_list<Type...>, Other>
    : std::bool_constant<(std::is_same_v<Type, Other> || ...)> {};

template<typename List, typename Type>
inline constexpr bool type_list_contains_v = type_list_contains<List, Type>::value;

// --- uniqueness, twice -----------------------------------------------------
//
// Exercise 2 asks for two implementations because they do not cost the same.
// Both keep the *first* occurrence and preserve order.

namespace internal {

// (a) Recursive, EnTT's shape: walk the input one element at a time, carrying
// the accumulated result as a trailing pack. Instantiation depth grows with the
// list length, because each step's type depends on the next step's.
template<typename, typename...>
struct type_list_unique_recursive;

template<typename First, typename... Other, typename... Kept>
struct type_list_unique_recursive<type_list<First, Other...>, Kept...>
    : std::conditional_t<(std::is_same_v<First, Kept> || ...),
                         type_list_unique_recursive<type_list<Other...>, Kept...>,
                         type_list_unique_recursive<type_list<Other...>, Kept..., First>> {};

template<typename... Kept>
struct type_list_unique_recursive<type_list<>, Kept...> {
    using type = type_list<Kept...>;
};

// (b) Fold-expression. The accumulator is a type carried through a left fold
// over a real binary operator, declared but never defined -- it is only ever
// named inside decltype. One instantiation of push_back_unique per element, and
// the fold itself is not recursive, so depth stays flat.
template<typename, typename>
struct push_back_unique;

template<typename... Kept, typename Type>
struct push_back_unique<type_list<Kept...>, Type> {
    using type = std::conditional_t<(std::is_same_v<Kept, Type> || ...),
                                    type_list<Kept...>,
                                    type_list<Kept..., Type>>;
};

template<typename List>
struct unique_fold {
    using type = List;
};

template<typename List, typename Type>
auto operator+(unique_fold<List>, std::type_identity<Type>)
    -> unique_fold<typename push_back_unique<List, Type>::type>;

template<typename>
struct type_list_unique_folded;

template<typename... Type>
struct type_list_unique_folded<type_list<Type...>> {
    using type = decltype((unique_fold<type_list<>>{} + ... + std::type_identity<Type>{}))::type;
};

} // namespace internal

template<typename List>
using type_list_unique_recursive_t = internal::type_list_unique_recursive<List>::type;

template<typename List>
using type_list_unique_folded_t = internal::type_list_unique_folded<List>::type;

// The one the rest of the project uses. See modules/02-traits/NOTES.md for the
// measurement behind the choice: same answer, flat instantiation depth.
template<typename List>
using type_list_unique_t = type_list_unique_folded_t<List>;

// --- difference ------------------------------------------------------------

template<typename...>
struct type_list_diff;

template<typename... Type, typename... Other>
struct type_list_diff<type_list<Type...>, type_list<Other...>> {
    using type = type_list_cat_t<
        std::conditional_t<type_list_contains_v<type_list<Other...>, Type>, type_list<>, type_list<Type>>...>;
};

template<typename... List>
using type_list_diff_t = type_list_diff<List...>::type;

// --- the vocabulary Modules 3, 6 and 7 consume -----------------------------

/**
 * Empty and not final, therefore usable as a base class purely for layout.
 * `final` is the trap: an empty final class is still empty, and inheriting from
 * it is ill-formed, so is_empty_v alone is not the question you meant to ask.
 */
template<typename Type>
struct is_ebco_eligible: std::bool_constant<std::is_empty_v<Type> && !std::is_final_v<Type>> {};

template<typename Type>
inline constexpr bool is_ebco_eligible_v = is_ebco_eligible<Type>::value;

/** Propagate const-ness from one type to another. All over iterator code. */
template<typename To, typename From>
struct constness_as {
    using type = std::remove_const_t<To>;
};

template<typename To, typename From>
struct constness_as<To, const From> {
    using type = const To;
};

template<typename To, typename From>
using constness_as_t = constness_as<To, From>::type;

/** Recover `C` from `R (C::*)(Args...)`, including the cv/ref-qualified forms. */
template<typename>
class member_class;

template<typename Type, typename Class, typename... Args>
class member_class<Type (Class::*)(Args...)> {
public:
    using type = Class;
};

template<typename Type, typename Class, typename... Args>
class member_class<Type (Class::*)(Args...) const> {
public:
    using type = Class;
};

template<typename Type, typename Class, typename... Args>
class member_class<Type (Class::*)(Args...) noexcept> {
public:
    using type = Class;
};

template<typename Type, typename Class, typename... Args>
class member_class<Type (Class::*)(Args...) const noexcept> {
public:
    using type = Class;
};

template<typename Type, typename Class>
class member_class<Type Class::*> {
public:
    using type = Class;
};

template<typename Member>
using member_class_t = member_class<Member>::type;

} // namespace acpp

#endif // ACPP_TYPE_TRAITS_HPP
