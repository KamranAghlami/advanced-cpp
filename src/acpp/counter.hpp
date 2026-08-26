#ifndef ACPP_COUNTER_HPP
#define ACPP_COUNTER_HPP

// Module 2, exercise 3 -- a macro-controlled policy converted into a trait.
//
// Before (Module 1's config.hpp):
//
//   #if defined ACPP_NO_ATOMIC
//   #    define ACPP_MAYBE_ATOMIC(Type) Type
//   #else
//   #    define ACPP_MAYBE_ATOMIC(Type) std::atomic<Type>
//   #endif
//
// One macro, one global answer for the whole build, and no way for a caller to
// disagree without recompiling everything. After: an inferred default, an inline
// opt-in, and a specialization -- the same ladder component_traits uses.
//
//   1. inferred   atomic iff the implementation says the build is threaded
//   2. opt in     `static constexpr bool atomic_counter = false;` on the tag
//   3. override   specialize acpp::counter_traits<Tag>
//
// ACPP_NO_ATOMIC survives as a way to move the *default*, which is what a build
// flag should have been doing all along.

#include <atomic>
#include <concepts>
#include <type_traits>

#include "config.hpp"

namespace acpp {

namespace internal {

// __STDCPP_THREADS__ is the standard's own answer to "can this program have more
// than one thread of execution", so it is the right thing to infer from. A
// freestanding single-threaded target answers no and pays nothing.
template<typename Tag>
struct atomic_counter
    : std::bool_constant<
#if defined ACPP_NO_ATOMIC
          false
#elif defined __STDCPP_THREADS__
          true
#elif defined _MSC_VER
          // MSVC does not define __STDCPP_THREADS__ at all -- it ships no
          // freestanding single-threaded mode for the macro to distinguish. Its
          // hosted runtime is always threaded, so reading the macro's absence as
          // "not threaded" would infer a non-atomic counter for every MSVC
          // build. Wrong in the one direction that costs correctness rather
          // than speed.
          true
#else
          false
#endif
          > {};

template<typename Tag>
    requires std::convertible_to<decltype(Tag::atomic_counter), bool>
struct atomic_counter<Tag>: std::bool_constant<Tag::atomic_counter> {};

} // namespace internal

template<typename Tag>
struct counter_traits {
    static constexpr bool atomic = internal::atomic_counter<Tag>::value;
};

/**
 * A monotonically increasing counter, one per tag type.
 *
 * ACPP_API for the same reason type_index has it, and with the same caveat: on
 * GCC an instantiation's visibility is the minimum over the template and its
 * arguments, so `Tag` must carry ACPP_EXPORT too if the counter is meant to be
 * shared across a shared-object boundary. See modules/01-type-identity/NOTES.md.
 */
template<typename Tag>
struct ACPP_API sequential_counter final {
    using value_type = id_type;
    static constexpr bool is_atomic = counter_traits<Tag>::atomic;

    [[nodiscard]] static id_type next() noexcept {
        static std::conditional_t<is_atomic, std::atomic<id_type>, id_type> value{};
        return value++;
    }
};

} // namespace acpp

#endif // ACPP_COUNTER_HPP
