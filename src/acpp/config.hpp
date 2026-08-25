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
// initialiser, not the counter they share. Non-atomic is defensible in a
// single-threaded build, so it is a knob rather than a decision.
#if defined ACPP_NO_ATOMIC
#    define ACPP_MAYBE_ATOMIC(Type) Type
#else
#    include <atomic>
#    define ACPP_MAYBE_ATOMIC(Type) ::std::atomic<Type>
#endif

#include <cstdint>

namespace acpp {

// 32 bits, spelled exactly, so the FNV-1a parameter table in hashed_string.hpp
// matches by type rather than by luck of what `unsigned` happens to be.
using id_type = std::uint32_t;

} // namespace acpp

#endif // ACPP_CONFIG_HPP
