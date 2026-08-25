#ifndef ACPP_COMPRESSED_PAIR_HPP
#define ACPP_COMPRESSED_PAIR_HPP

// Module 3 -- layout economy.
//
// A pair that costs nothing for the empty half. Every allocator-aware container
// in a library like this stores its allocator, deleter, comparator or hash in
// one of these; all four are usually empty, and without compression a small
// container pays 8-32 bytes per instance for objects with no state.

#include "stl/cstddef.hpp"
#include "stl/tuple.hpp"
#include "stl/type_traits.hpp"
#include "stl/utility.hpp"
#include "type_traits.hpp"

namespace acpp {

namespace internal {

// ---------------------------------------------------------------------------
// The element. Two implementations, one interface.
//
// The `Tag` parameter is not decoration and it is the module's checkpoint:
// without it, compressed_pair<empty, empty> would inherit from the same base
// twice, which is ill-formed. Tagging with 0u and 1u makes the two bases
// distinct types. modules/03-layout-economy/tagless_pair_fails.cpp compiles the
// counterexample and the build requires it to fail.
// ---------------------------------------------------------------------------
template<typename Type, stl::size_t>
struct compressed_pair_element {
    using reference = Type &;
    using const_reference = const Type &;

    constexpr compressed_pair_element()
        noexcept(stl::is_nothrow_default_constructible_v<Type>)
        requires stl::is_default_constructible_v<Type>
        : value{} {}

    // Parenthesised, not braced. Braces would turn `Type{3u, 'a'}` into a
    // narrowing error or, worse, an initializer_list overload nobody wanted;
    // C++20's parenthesised aggregate initialisation (P0960) means parentheses
    // still work for aggregates, so there is nothing left for braces to buy.
    template<typename Arg>
        requires(!stl::is_same_v<stl::remove_cvref_t<Arg>, compressed_pair_element>)
    constexpr explicit compressed_pair_element(Arg &&arg)
        noexcept(stl::is_nothrow_constructible_v<Type, Arg>)
        : value(stl::forward<Arg>(arg)) {}

    template<typename... Args, stl::size_t... Index>
    constexpr compressed_pair_element(stl::tuple<Args...> args, stl::index_sequence<Index...>)
        noexcept(stl::is_nothrow_constructible_v<Type, Args...>)
        : value(stl::forward<Args>(stl::get<Index>(args))...) {}

    [[nodiscard]] constexpr reference get() noexcept { return value; }
    [[nodiscard]] constexpr const_reference get() const noexcept { return value; }

private:
    Type value;
};

// The compressing half: inherit instead of storing. `get()` returns *this, so
// the two implementations expose the same interface and compressed_pair itself
// is written once.
template<typename Type, stl::size_t Tag>
    requires is_ebco_eligible_v<Type>
struct compressed_pair_element<Type, Tag>: Type {
    using reference = Type &;
    using const_reference = const Type &;
    using base_type = Type;

    constexpr compressed_pair_element()
        noexcept(stl::is_nothrow_default_constructible_v<base_type>)
        requires stl::is_default_constructible_v<base_type>
        : base_type{} {}

    template<typename Arg>
        requires(!stl::is_same_v<stl::remove_cvref_t<Arg>, compressed_pair_element>)
    constexpr explicit compressed_pair_element(Arg &&arg)
        noexcept(stl::is_nothrow_constructible_v<base_type, Arg>)
        : base_type(stl::forward<Arg>(arg)) {}

    template<typename... Args, stl::size_t... Index>
    constexpr compressed_pair_element(stl::tuple<Args...> args, stl::index_sequence<Index...>)
        noexcept(stl::is_nothrow_constructible_v<base_type, Args...>)
        : base_type(stl::forward<Args>(stl::get<Index>(args))...) {}

    [[nodiscard]] constexpr reference get() noexcept { return *this; }
    [[nodiscard]] constexpr const_reference get() const noexcept { return *this; }
};

} // namespace internal

template<typename First, typename Second>
class compressed_pair final
    : internal::compressed_pair_element<First, 0u>,
      internal::compressed_pair_element<Second, 1u> {
    using first_base = internal::compressed_pair_element<First, 0u>;
    using second_base = internal::compressed_pair_element<Second, 1u>;

public:
    using first_type = First;
    using second_type = Second;

    constexpr compressed_pair()
        noexcept(stl::is_nothrow_default_constructible_v<first_base>
                 && stl::is_nothrow_default_constructible_v<second_base>)
        requires(stl::is_default_constructible_v<First> && stl::is_default_constructible_v<Second>)
        : first_base{}, second_base{} {}

    constexpr compressed_pair(const compressed_pair &) = default;
    constexpr compressed_pair(compressed_pair &&) noexcept = default;
    constexpr compressed_pair &operator=(const compressed_pair &) = default;
    constexpr compressed_pair &operator=(compressed_pair &&) noexcept = default;
    constexpr ~compressed_pair() = default;

    template<typename Arg, typename Other>
    constexpr compressed_pair(Arg &&arg, Other &&other)
        noexcept(stl::is_nothrow_constructible_v<first_base, Arg>
                 && stl::is_nothrow_constructible_v<second_base, Other>)
        : first_base{stl::forward<Arg>(arg)}, second_base{stl::forward<Other>(other)} {}

    // Piecewise construction: the only way to build a pair whose halves are
    // themselves not movable or not copyable.
    template<typename... Args, typename... Other>
    constexpr compressed_pair(stl::piecewise_construct_t, stl::tuple<Args...> args, stl::tuple<Other...> other)
        : first_base{stl::move(args), stl::index_sequence_for<Args...>{}},
          second_base{stl::move(other), stl::index_sequence_for<Other...>{}} {}

    [[nodiscard]] constexpr first_base::reference first() noexcept { return first_base::get(); }
    [[nodiscard]] constexpr first_base::const_reference first() const noexcept { return first_base::get(); }
    [[nodiscard]] constexpr second_base::reference second() noexcept { return second_base::get(); }
    [[nodiscard]] constexpr second_base::const_reference second() const noexcept { return second_base::get(); }

    constexpr void swap(compressed_pair &other)
        noexcept(stl::is_nothrow_swappable_v<First> && stl::is_nothrow_swappable_v<Second>) {
        using stl::swap;
        swap(first(), other.first());
        swap(second(), other.second());
    }

    // Structured bindings. get<N> has to be a member (or a free function found
    // by ADL) plus tuple_size/tuple_element specializations -- see below.
    template<stl::size_t Index>
    [[nodiscard]] constexpr decltype(auto) get() noexcept {
        if constexpr(Index == 0u) {
            return first();
        } else {
            static_assert(Index == 1u, "a pair has two elements");
            return second();
        }
    }

    template<stl::size_t Index>
    [[nodiscard]] constexpr decltype(auto) get() const noexcept {
        if constexpr(Index == 0u) {
            return first();
        } else {
            static_assert(Index == 1u, "a pair has two elements");
            return second();
        }
    }
};

template<typename First, typename Second>
compressed_pair(First &&, Second &&) -> compressed_pair<stl::decay_t<First>, stl::decay_t<Second>>;

template<typename First, typename Second>
constexpr void swap(compressed_pair<First, Second> &lhs, compressed_pair<First, Second> &rhs)
    noexcept(noexcept(lhs.swap(rhs))) {
    lhs.swap(rhs);
}

// ---------------------------------------------------------------------------
// The C++20 spelling of the same idea, for comparison. Far less code -- and the
// reason EnTT still ships the inheritance version is that MSVC's ABI ignores
// [[no_unique_address]] unless you spell it [[msvc::no_unique_address]].
// modules/03-layout-economy/NOTES.md has the measured sizes.
// ---------------------------------------------------------------------------
template<typename First, typename Second>
struct nua_pair {
    using first_type = First;
    using second_type = Second;

    [[no_unique_address]] First first_value{};
    [[no_unique_address]] Second second_value{};

    [[nodiscard]] constexpr First &first() noexcept { return first_value; }
    [[nodiscard]] constexpr const First &first() const noexcept { return first_value; }
    [[nodiscard]] constexpr Second &second() noexcept { return second_value; }
    [[nodiscard]] constexpr const Second &second() const noexcept { return second_value; }
};

} // namespace acpp

// Structured-binding support.
//
// These two stay spelled `std::`, and that is the seam's floor rather than an
// oversight: the *language* names std::tuple_size and std::tuple_element when it
// expands a structured binding. Same for std::initializer_list and the coroutine
// traits. A freestanding shim can replace everything a library merely calls; it
// cannot replace what the core language reaches for by name.
namespace std {

template<typename First, typename Second>
struct tuple_size<acpp::compressed_pair<First, Second>>: integral_constant<size_t, 2u> {};

template<size_t Index, typename First, typename Second>
struct tuple_element<Index, acpp::compressed_pair<First, Second>>
    : conditional<Index == 0u, First, Second> {};

} // namespace std

#endif // ACPP_COMPRESSED_PAIR_HPP
