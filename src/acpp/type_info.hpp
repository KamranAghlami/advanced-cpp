#ifndef ACPP_TYPE_INFO_HPP
#define ACPP_TYPE_INFO_HPP

// Module 1 -- type identity without RTTI.
//
// Four separate techniques live in this header. In order:
//   1.1  the type's spelling, sliced out of __PRETTY_FUNCTION__
//   1.2  the int/char overload ranking that picks consteval-if-possible
//   1.3  a sequential ID per type, and the cross-DSO trap that comes with it
//   1.4  the hash (in hashed_string.hpp, used here)

#include <string_view>
#include <type_traits>
#include <utility>

#include "config.hpp"
#include "hashed_string.hpp"

namespace acpp {

namespace internal {

// 1.3 -- the shared counter. Declared here, defined in type_index.cpp: a
// header-only counter is a different object in every DSO that instantiates it.
// One TU plus ACPP_API is what makes the IDs process-wide.
struct ACPP_API type_index final {
    [[nodiscard]] static id_type next() noexcept;
};

template<typename Type>
[[nodiscard]] constexpr const char *pretty_function() noexcept {
#if defined ACPP_PRETTY_FUNCTION
    return static_cast<const char *>(ACPP_PRETTY_FUNCTION);
#else
    return "";
#endif
}

// 1.1 -- slice the type name out of the signature string.
//
// GCC:   constexpr const char* acpp::internal::pretty_function() [with Type = std::vector<int>]
// Clang: const char *acpp::internal::pretty_function() [Type = std::vector<int>]
// MSVC:  const char *__cdecl acpp::internal::pretty_function<class std::vector<int> >(void)
//
// Find the prefix character, skip the spaces after it, cut at the LAST suffix
// character -- last, not first, because the type's own spelling can contain it
// (`std::array<int, 4>` under MSVC, any `[N]` under GCC).
template<typename Type>
[[nodiscard]] constexpr auto stripped_type_name() noexcept {
#if defined ACPP_PRETTY_FUNCTION
    const std::string_view full_name{pretty_function<Type>()};
    const auto first = full_name.find_first_not_of(' ', full_name.find_first_of(ACPP_PRETTY_FUNCTION_PREFIX) + 1u);
    return full_name.substr(first, full_name.find_last_of(ACPP_PRETTY_FUNCTION_SUFFIX) - first);
#else
    return std::string_view{};
#endif
}

// 1.2 -- "constexpr if possible, runtime if not", chosen by overload ranking.
//
// Two things happen at once, and both are load-bearing:
//
//   * `auto = stripped_type_name<Type>().find_first_of('.')` is a non-type
//     template parameter with a default, so the slice MUST be constant-evaluated
//     during template argument deduction. If it cannot be -- some compilers spell
//     local types and lambdas in ways that break the extraction, and substr on a
//     bad index throws -- that is a substitution failure, and the overload simply
//     drops out. if constexpr cannot do this job: the failure is *inside* the
//     constant evaluation, so there is nothing to test in a condition.
//
//   * `int` vs `char` breaks the tie when both are viable. The call site passes
//     the literal 0: an exact match for int, an integral conversion for char.
//     Exact match wins, deterministically, with no ordering rules to remember.
//
// Remove the `auto =` parameter and the first overload becomes unconditionally
// viable: types that cannot be spelled at compile time become hard errors
// instead of falling back.
template<typename Type, auto = stripped_type_name<Type>().find_first_of('.')>
[[nodiscard]] ACPP_CONSTEVAL std::string_view type_name(int) noexcept {
    constexpr auto value = stripped_type_name<Type>();
    return value;
}

template<typename Type>
[[nodiscard]] std::string_view type_name(char) noexcept {
    // Function-local static, not a constexpr: this branch exists precisely
    // because the value could not be produced at compile time. Computed once.
    static const auto value = stripped_type_name<Type>();
    return value;
}

template<typename Type, auto = stripped_type_name<Type>().find_first_of('.')>
[[nodiscard]] ACPP_CONSTEVAL id_type type_hash(int) noexcept {
    constexpr auto stripped = stripped_type_name<Type>();
    constexpr auto value = hashed_string::value(stripped.data(), stripped.size());
    return value;
}

template<typename Type>
[[nodiscard]] id_type type_hash(char) noexcept {
    static const id_type value = [](const auto stripped) {
        return hashed_string::value(stripped.data(), stripped.size());
    }(stripped_type_name<Type>());
    return value;
}

} // namespace internal

/**
 * Sequential identifier: 0, 1, 2, ... in first-touch order. Dense enough to
 * index an array directly, and not stable across runs, builds or link orders --
 * never serialise it. ACPP_API is load-bearing; see odr_across_dso.
 */
template<typename Type>
struct ACPP_API type_index final {
    [[nodiscard]] static id_type value() noexcept {
        static const id_type value = internal::type_index::next();
        return value;
    }

    [[nodiscard]] constexpr operator id_type() const noexcept {
        return value();
    }
};

/**
 * Hash of the type's spelling: stable for a given compiler, and available at
 * compile time whenever the name was, which is what makes it usable as a case
 * label. Falls back to type_index where there is no signature macro at all.
 */
template<typename Type>
struct type_hash final {
#if defined ACPP_PRETTY_FUNCTION
    [[nodiscard]] static constexpr id_type value() noexcept {
        return internal::type_hash<Type>(0);
    }
#else
    [[nodiscard]] static id_type value() noexcept {
        return type_index<Type>::value();
    }
#endif

    [[nodiscard]] constexpr operator id_type() const noexcept {
        return value();
    }
};

template<typename Type>
struct type_name final {
    [[nodiscard]] static constexpr std::string_view value() noexcept {
        return internal::type_name<Type>(0);
    }

    [[nodiscard]] constexpr operator std::string_view() const noexcept {
        return value();
    }
};

/** The runtime face of the above: a copyable, comparable descriptor. */
class type_info final {
public:
    constexpr type_info() noexcept = default;

    template<typename Type>
    constexpr type_info(std::in_place_type_t<Type>) noexcept
        : seq{type_index<Type>::value()},
          identifier{type_hash<Type>::value()},
          alias{type_name<Type>::value()} {}

    [[nodiscard]] constexpr id_type index() const noexcept { return seq; }
    [[nodiscard]] constexpr id_type hash() const noexcept { return identifier; }
    [[nodiscard]] constexpr std::string_view name() const noexcept { return alias; }

    [[nodiscard]] constexpr bool operator==(const type_info &other) const noexcept {
        return hash() == other.hash();
    }

private:
    id_type seq{};
    id_type identifier{};
    std::string_view alias{};
};

template<typename Type>
[[nodiscard]] const type_info &type_id() noexcept {
    static const type_info instance{std::in_place_type<std::remove_cv_t<std::remove_reference_t<Type>>>};
    return instance;
}

} // namespace acpp

#endif // ACPP_TYPE_INFO_HPP
