#ifndef ACPP_HASHED_STRING_HPP
#define ACPP_HASHED_STRING_HPP

// Module 1.4 -- compile-time FNV-1a, and the user-defined literal that turns a
// string literal into an integer ID with no runtime work at all.
//
// The technique transfers directly to command dispatch, config keys and protocol
// tags on constrained targets: you keep the human-readable spelling in the source
// and ship the integer.

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "config.hpp"

namespace acpp {

namespace internal {

template<typename>
struct fnv1a_params;

template<>
struct fnv1a_params<std::uint32_t> {
    static constexpr std::uint32_t offset = 2166136261u;
    static constexpr std::uint32_t prime = 16777619u;
};

template<>
struct fnv1a_params<std::uint64_t> {
    static constexpr std::uint64_t offset = 14695981039346656037ull;
    static constexpr std::uint64_t prime = 1099511628211ull;
};

} // namespace internal

/**
 * A string reduced to its hash at compile time.
 *
 * Non-owning by design: it holds the pointer the literal already had. That is
 * what makes it free, and also what makes storing one past the lifetime of a
 * non-literal source a bug.
 *
 * COLLISIONS ARE POSSIBLE and this type does not pretend otherwise. FNV-1a over
 * 32 bits gives roughly a 50% chance of *some* collision at ~77k distinct
 * strings (birthday bound). Before copying this pattern, price the collision:
 *   - dispatch table   -> wrong command executed. Unacceptable; assert uniqueness
 *                         of the closed set at compile time (see cmd_dispatch).
 *   - cache key        -> a wrong hit. Usually unacceptable; verify the string.
 *   - debug label      -> a confusing log line. Fine.
 * The library's job is to be fast; deciding what a collision costs is yours.
 */
template<typename Char>
class basic_hashed_string {
    using params = internal::fnv1a_params<id_type>;

    // Non-explicit on purpose: it exists so the (const Char *) constructor can be
    // constrained to *literals* while a runtime pointer still has a way in.
    struct const_wrapper {
        constexpr const_wrapper(const Char *str) noexcept
            : repr{str} {}

        const Char *repr;
    };

    [[nodiscard]] static constexpr id_type fold(const Char *str, const std::size_t len) noexcept {
        id_type hash = params::offset;

        for(std::size_t pos = 0u; pos < len; ++pos) {
            hash = (hash ^ static_cast<id_type>(static_cast<unsigned char>(str[pos]))) * params::prime;
        }

        return hash;
    }

public:
    using value_type = Char;
    using size_type = std::size_t;
    using hash_type = id_type;

    [[nodiscard]] static constexpr hash_type value(const value_type *str, const size_type len) noexcept {
        return fold(str, len);
    }

    // Array overload: N-1 drops the terminator. Preferred over const_wrapper for
    // literals because the length is known without a strlen.
    template<std::size_t N>
    [[nodiscard]] static constexpr hash_type value(const value_type (&str)[N]) noexcept {
        return fold(str, N - 1u);
    }

    [[nodiscard]] static constexpr hash_type value(const_wrapper wrapper) noexcept {
        size_type len = 0u;
        while(wrapper.repr[len] != value_type{}) { ++len; }
        return fold(wrapper.repr, len);
    }

    constexpr basic_hashed_string() noexcept = default;

    constexpr basic_hashed_string(const value_type *str, const size_type len) noexcept
        : repr{str}, length{len}, hash{fold(str, len)} {}

    template<std::size_t N>
    constexpr basic_hashed_string(const value_type (&str)[N]) noexcept
        : basic_hashed_string{static_cast<const value_type *>(str), N - 1u} {}

    constexpr explicit basic_hashed_string(const_wrapper wrapper) noexcept
        : repr{wrapper.repr}, length{0u}, hash{params::offset} {
        while(repr[length] != value_type{}) { ++length; }
        hash = fold(repr, length);
    }

    [[nodiscard]] constexpr const value_type *data() const noexcept { return repr; }
    [[nodiscard]] constexpr size_type size() const noexcept { return length; }
    [[nodiscard]] constexpr hash_type value() const noexcept { return hash; }
    [[nodiscard]] constexpr operator hash_type() const noexcept { return hash; }

    [[nodiscard]] constexpr bool operator==(const basic_hashed_string &other) const noexcept {
        return hash == other.hash;
    }

private:
    const value_type *repr{};
    size_type length{};
    hash_type hash{params::offset};
};

using hashed_string = basic_hashed_string<char>;
using hashed_wstring = basic_hashed_string<wchar_t>;

inline namespace literals {

[[nodiscard]] constexpr hashed_string operator""_hs(const char *str, std::size_t len) noexcept {
    return hashed_string{str, len};
}

[[nodiscard]] constexpr hashed_wstring operator""_hws(const wchar_t *str, std::size_t len) noexcept {
    return hashed_wstring{str, len};
}

} // namespace literals

} // namespace acpp

#endif // ACPP_HASHED_STRING_HPP
