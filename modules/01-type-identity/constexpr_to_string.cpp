// Module 1, exercise 4 -- the int/char ranking trick, applied elsewhere.
//
// Problem: to_string(T) should use an allocation-free, constant-evaluable path
// when T admits one, and fall back to iostreams when it does not. The caller
// writes one name; the compiler picks.
//
// Why not `if constexpr`: the condition would have to be "does formatting T at
// compile time succeed", and the failure it needs to detect happens *inside* the
// constant evaluation. There is nothing to test -- there is only an evaluation
// that either is or is not a constant expression. That is what the defaulted
// non-type template parameter probes, and why the trick exists.
//
// Why not a concept: a concept can require that an expression is well-formed,
// but not that it is a *constant* expression. `requires { stringify(T{}); }` is
// satisfied by a type whose stringify only works at runtime.

#include <array>
#include <cstddef>
#include <cstdint>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>

#include <acpp/testing.hpp>

namespace {

// A structural type: every member public, no user-provided special members. That
// is the requirement for using a value of it as a non-type template argument,
// which is what the probe below needs.
template<std::size_t N>
struct fixed_string {
    std::array<char, N> data{};
    std::size_t length{};

    [[nodiscard]] constexpr std::string_view view() const noexcept {
        return std::string_view{data.data(), length};
    }

    [[nodiscard]] constexpr bool operator==(const std::string_view other) const noexcept {
        return view() == other;
    }
};

using small_string = fixed_string<24>;

// The opt-in hook: a type joins the constexpr path by providing this member.
// Same customization-point idea Module 2 formalises -- inline opt-in, no
// specialization in someone else's namespace.
template<typename Type>
concept self_describing = requires(const Type &value) {
    { value.acpp_to_string() } -> std::convertible_to<small_string>;
};

[[nodiscard]] constexpr small_string stringify(bool value) noexcept {
    small_string out{};
    for(const char c: (value ? std::string_view{"true"} : std::string_view{"false"})) {
        out.data[out.length++] = c;
    }
    return out;
}

template<typename Type>
    requires std::is_integral_v<Type> && (!std::is_same_v<Type, bool>)
[[nodiscard]] constexpr small_string stringify(Type value) noexcept {
    small_string out{};
    const bool negative = std::is_signed_v<Type> && value < Type{};

    // Accumulate through the unsigned type so the most negative value does not
    // overflow on negation -- the classic itoa bug, and free to avoid here.
    auto magnitude = static_cast<std::make_unsigned_t<Type>>(negative ? Type{} - value : value);
    char digits[24]{};
    std::size_t count = 0u;

    do {
        digits[count++] = static_cast<char>('0' + (magnitude % 10u));
        magnitude /= 10u;
    } while(magnitude != 0u);

    if(negative) {
        out.data[out.length++] = '-';
    }

    while(count != 0u) {
        out.data[out.length++] = digits[--count];
    }

    return out;
}

template<typename Type>
    requires std::is_enum_v<Type>
[[nodiscard]] constexpr small_string stringify(Type value) noexcept {
    return stringify(static_cast<std::underlying_type_t<Type>>(value));
}

template<self_describing Type>
[[nodiscard]] constexpr small_string stringify(const Type &value) noexcept {
    return value.acpp_to_string();
}

namespace internal {

// Preferred. The defaulted NTTP forces stringify(Type{}) to be constant
// evaluated during deduction; if it cannot be -- no overload, not a literal
// type, not default-constructible at compile time -- that is a substitution
// failure and this candidate silently disappears.
template<typename Type, auto = stringify(Type{})>
[[nodiscard]] constexpr small_string to_string(const Type &value, int) noexcept {
    return stringify(value);
}

// Fallback. Allocates, cannot run at compile time, works on nearly everything.
template<typename Type>
[[nodiscard]] std::string to_string(const Type &value, char) {
    std::ostringstream stream;
    stream << value;
    return stream.str();
}

} // namespace internal

// One name at the call site. `auto` return: the two paths deliberately return
// different types, because a caller on the constexpr path should not be handed
// a std::string it did not need.
template<typename Type>
[[nodiscard]] constexpr auto to_string(const Type &value) {
    return internal::to_string(value, 0);
}

// --- types to exercise both paths -----------------------------------------

enum class mode : std::uint8_t { idle = 3, active = 7 };

struct version {
    int major{};
    int minor{};

    [[nodiscard]] constexpr small_string acpp_to_string() const noexcept {
        small_string out = stringify(major);
        out.data[out.length++] = '.';
        for(const char c: stringify(minor).view()) {
            out.data[out.length++] = c;
        }
        return out;
    }
};

// No acpp_to_string, but streamable: must take the fallback.
struct point {
    int x{};
    int y{};
};

std::ostream &operator<<(std::ostream &stream, const point &value) {
    return stream << '(' << value.x << ", " << value.y << ')';
}

// The proof that selection happened at compile time and went the right way.
static_assert(to_string(42) == "42");
static_assert(to_string(-7) == "-7");
static_assert(to_string(true) == "true");
static_assert(to_string(mode::active) == "7");
static_assert(to_string(version{4, 1}) == "4.1");

static_assert(std::is_same_v<decltype(to_string(42)), small_string>);
static_assert(std::is_same_v<decltype(to_string(point{})), std::string>);
static_assert(std::is_same_v<decltype(to_string(std::string{})), std::string>);

// The most negative value, at compile time, through the unsigned accumulator.
static_assert(to_string(std::int8_t{-128}) == "-128");

} // namespace

int main() {
    acpp::testing::suite suite{"module 01 / constexpr_to_string"};

    suite.check(to_string(1234) == "1234", "constexpr path, runtime value");
    suite.check(to_string(mode::idle) == "3", "enum goes through its underlying type");
    suite.check(to_string(version{1, 20}) == "1.20", "opt-in member selects the constexpr path");
    suite.check(to_string(point{2, 3}) == "(2, 3)", "streamable-only type falls back");
    suite.check(to_string(std::string{"hello"}) == "hello", "non-literal type falls back");

    // The fallback is not a lesser answer, it is a different one: it allocates.
    // Knowing which path a call took is the point of keeping the return types
    // distinct, so this is a compile-time question and not a runtime one.
    suite.check(std::is_same_v<decltype(to_string(point{})), std::string>,
                "the caller can tell which path was taken, from the type alone");

    return suite.report();
}
