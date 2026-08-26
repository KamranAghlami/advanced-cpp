#ifndef ACPP_CONFIG_HPP
#define ACPP_CONFIG_HPP

// Module 1 -- the macro layer EnTT keeps in config/config.h.
//
// Each macro is annotated with the failure it prevents. If you cannot name the
// failure, the macro should not exist.

// Compiler-generated type spellings (1.1). __PRETTY_FUNCTION__ / __FUNCSIG__
// embed the template argument's spelling in a string the compiler emits anyway.
// The delimiters differ per compiler, which is why these are macros.
#if defined __clang__ || defined __GNUC__
#    define ACPP_PRETTY_FUNCTION __PRETTY_FUNCTION__
#    define ACPP_PRETTY_FUNCTION_PREFIX '='
#    define ACPP_PRETTY_FUNCTION_SUFFIX ']'
#elif defined _MSC_VER
#    define ACPP_PRETTY_FUNCTION __FUNCSIG__
#    define ACPP_PRETTY_FUNCTION_PREFIX '<'
#    define ACPP_PRETTY_FUNCTION_SUFFIX '>'
#endif

// consteval where available (1.2). The ranking trick wants the preferred
// overload to be *immediate*, so that selecting it is proof the name was
// computed at compile time. Without consteval this degrades to constexpr:
// weaker evidence, same behaviour.
#if defined __cpp_consteval
#    define ACPP_CONSTEVAL consteval
#else
#    define ACPP_CONSTEVAL constexpr
#endif

// Cross-DSO symbol merging (1.3). Prevents: a function-local static inside a
// class template inside a header being instantiated separately in every shared
// object, giving two IDs for one type and a registry that silently loses
// components. Default visibility puts the symbol in the dynamic symbol table so
// the loader collapses the definitions. modules/01-type-identity/NOTES.md has
// the measured details -- the attribute alone is not sufficient on GCC.
#if defined _WIN32 || defined __CYGWIN__
#    define ACPP_EXPORT __declspec(dllexport)
#    define ACPP_IMPORT __declspec(dllimport)
#    define ACPP_HIDDEN
#elif defined __GNUC__
#    define ACPP_EXPORT __attribute__((visibility("default")))
#    define ACPP_IMPORT __attribute__((visibility("default")))
#    define ACPP_HIDDEN __attribute__((visibility("hidden")))
#else
#    define ACPP_EXPORT
#    define ACPP_IMPORT
#    define ACPP_HIDDEN
#endif

#if defined ACPP_API_EXPORT
#    define ACPP_API ACPP_EXPORT
#elif defined ACPP_API_IMPORT
#    define ACPP_API ACPP_IMPORT
#else
#    define ACPP_API ACPP_EXPORT
#endif

// Concurrent first-touch of the counter (1.3). Two threads asking for two
// *different* types both call next(). The static-local guard serialises each
// initialiser, not the counter they share.
//
// This started life as an ACPP_MAYBE_ATOMIC(Type) macro. Module 2's exercise 3
// replaced it with acpp::counter_traits<Tag>, which infers the answer from
// __STDCPP_THREADS__ and lets a caller disagree per counter. ACPP_NO_ATOMIC
// still moves the default; it no longer *is* the policy. See counter.hpp.

// ---------------------------------------------------------------------------
// Stop now, loudly.
//
// Used where an invariant the type system cannot express has been violated --
// relocating a type that cannot be relocated, or overflowing a fixed-capacity
// container on a target that chose one to avoid the heap. A trap is a stack
// trace; std::unreachable() is silent corruption; an exception may not exist on
// the target at all.
// ---------------------------------------------------------------------------
#if defined _MSC_VER && !defined __clang__
#    define ACPP_TRAP() __debugbreak()
#else
#    define ACPP_TRAP() __builtin_trap()
#endif

// Cache line size, for the alignas that keeps contended atomics apart
// (Modules 9-11). Hard-coded rather than
// std::hardware_destructive_interference_size, which GCC warns about using
// across an ABI boundary and which is 64 on every target this project builds
// for. If that stops being true, this is the one line to change.
#ifndef ACPP_CACHELINE_SIZE
#    define ACPP_CACHELINE_SIZE 64
#endif

// Storage tunables (Modules 2, 6, 7). Page sizes must be powers of two: the
// paged indirection replaces a division with a shift and a mask, and both the
// sparse array and the payload assert it at compile time.
#ifndef ACPP_PACKED_PAGE
#    define ACPP_PACKED_PAGE 1024
#endif

#ifndef ACPP_SPARSE_PAGE
#    define ACPP_SPARSE_PAGE 4096
#endif

// Empty type optimisation. page_size asks `is_empty_v<ACPP_ETO_TYPE(Type)>`, so
// substituting `void` -- which is never empty -- turns the optimisation off for
// every type at once and gives empty components a real, zero-information payload
// array. Occasionally what you want while debugging a layout question.
#if defined ACPP_NO_ETO
#    define ACPP_ETO_TYPE(Type) void
#else
#    define ACPP_ETO_TYPE(Type) Type
#endif

#include <cstdint>

namespace acpp {

// 32 bits, spelled exactly, so the FNV-1a parameter table in hashed_string.hpp
// matches by type rather than by luck of what `unsigned` happens to be.
using id_type = std::uint32_t;

// Whether [[no_unique_address]] actually removes an empty member's storage.
//
// The attribute is permission, not obligation, and MSVC declines it: its ABI
// ignores the standard spelling for backward compatibility and offers
// [[msvc::no_unique_address]] as the one that compresses. Measured by CI's msvc
// job on 2026-08-26 -- nua_pair<empty, int> is 8 bytes there against 4 under
// libstdc++ and libc++. It is the reason EnTT still ships the inheritance-based
// compressed_pair rather than replacing it with the attribute.
//
// Exposed here because two modules now have layout expectations that turn on it
// (Module 3's pair sizes, Module 12's partitioner), and because a header that
// uses the attribute for size should be able to say what it costs.
#if defined _MSC_VER && !defined __clang__
inline constexpr bool nua_compresses = false;
#else
inline constexpr bool nua_compresses = true;
#endif

} // namespace acpp

#endif // ACPP_CONFIG_HPP
