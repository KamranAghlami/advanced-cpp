#ifndef ACPP_BIT_HPP
#define ACPP_BIT_HPP

// Module 6 -- the three lines the paging scheme rests on.

#include "stl/bit.hpp"
#include "stl/concepts.hpp"
#include "stl/cstddef.hpp"

namespace acpp {

/**
 * Remainder by a power of two, as one AND.
 *
 * The assertion is the point of having a function at all: the whole paging
 * scheme depends on power-of-two page sizes, and `value & (mod - 1)` is silently
 * wrong for anything else. Stating it here means it is checked once, at compile
 * time, wherever a page size is chosen -- rather than discovered as corrupted
 * indices at run time.
 */
template<stl::unsigned_integral Type>
[[nodiscard]] constexpr Type fast_mod(const Type value, const stl::size_t mod) noexcept {
    return static_cast<Type>(value & static_cast<Type>(mod - 1u));
}

/** Compile-time guard for a page size, for use in a static_assert. */
[[nodiscard]] constexpr bool is_valid_page_size(const stl::size_t value) noexcept {
    return value != 0u && stl::has_single_bit(value);
}

} // namespace acpp

#endif // ACPP_BIT_HPP
