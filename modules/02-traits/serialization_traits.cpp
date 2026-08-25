// Module 2, exercise 1 -- the three-level customization ladder, built from
// scratch on a different problem.
//
//   1. inferred   trivially copyable types are byte-blittable, everything else
//                 needs a real visit
//   2. opt in     `static constexpr auto serialization = ...;` in your own type
//   3. override   specialize acpp::serialization_traits<Your>
//
// The library ships level 1. Levels 2 and 3 are the user's, and the point of the
// ladder is that each is more invasive than the last, so the common case costs
// nothing and the awkward case is still possible without a fork.

#include <concepts>
#include <cstdint>
#include <cstring>
#include <string>
#include <type_traits>
#include <vector>

#include <acpp/testing.hpp>
#include <acpp/type_traits.hpp>

namespace acpp {

enum class serialization_kind : std::uint8_t {
    blit,      // memcpy the object representation
    visit,     // call the type's own writer
    forbidden, // refuse at compile time
};

namespace internal {

// Level 1. A trivially copyable type has a meaningful object representation, so
// copying its bytes is a valid serialisation *of that object*. Note what this
// does not claim: it says nothing about whether the bytes mean the same thing on
// another machine. Endianness and padding are a separate decision, made once at
// the stream, not per type.
template<typename Type>
struct serialization
    : std::integral_constant<serialization_kind,
                             std::is_trivially_copyable_v<Type> ? serialization_kind::blit
                                                                : serialization_kind::visit> {};

// A raw pointer serialised by value is a bug that trivially-copyable would
// happily wave through. Refusing is the whole reason the inferred default is
// allowed to have exceptions.
template<typename Type>
    requires std::is_pointer_v<Type>
struct serialization<Type>: std::integral_constant<serialization_kind, serialization_kind::forbidden> {};

// Level 2: the inline opt-in.
template<typename Type>
    requires std::convertible_to<decltype(Type::serialization), serialization_kind>
struct serialization<Type>: std::integral_constant<serialization_kind, Type::serialization> {};

} // namespace internal

// Level 3: specialize this.
template<typename Type>
struct serialization_traits {
    using element_type = Type;
    static constexpr serialization_kind kind = internal::serialization<Type>::value;
    static constexpr bool stable_layout = std::is_standard_layout_v<Type> && (kind == serialization_kind::blit);
};

} // namespace acpp

namespace {

using acpp::serialization_kind;

// --- level 1: nothing written -----------------------------------------------

struct pod {
    int x;
    float y;
};

// --- level 2: opts in from inside itself ------------------------------------

// Trivially copyable, but the id is a process-local handle: blitting it would
// serialise a number that means nothing to the reader. The type knows that; the
// library cannot.
struct resource_ref {
    std::uint32_t id;
    static constexpr auto serialization = serialization_kind::visit;
};

// A type that must never leave the process at all.
struct secret_key {
    unsigned char bytes[32];
    static constexpr auto serialization = serialization_kind::forbidden;
};

// --- level 3: overridden from outside ---------------------------------------

// Third-party, unmodifiable, and the inferred answer is wrong: std::string is
// not trivially copyable, so it infers `visit`, which is correct -- so to make
// the exercise honest, override something whose inference is genuinely wrong.
struct legacy_blob {
    void *cursor;  // a pointer member, so the primary template would forbid it
    std::size_t len;
};

} // namespace

namespace acpp {

// The escape hatch: we know this blob's cursor is rebuilt on load, so blitting
// the length and ignoring the pointer is fine. Nothing short of a full
// specialization can express "the library's inference is wrong here".
template<>
struct serialization_traits<legacy_blob> {
    using element_type = legacy_blob;
    static constexpr serialization_kind kind = serialization_kind::blit;
    static constexpr bool stable_layout = true;
};

} // namespace acpp

namespace {

template<typename Type>
inline constexpr auto kind_of = acpp::serialization_traits<Type>::kind;

// All three rungs, checked at compile time.
static_assert(kind_of<pod> == serialization_kind::blit);
static_assert(kind_of<int> == serialization_kind::blit);
static_assert(kind_of<std::string> == serialization_kind::visit);
static_assert(kind_of<std::vector<int>> == serialization_kind::visit);
static_assert(kind_of<int *> == serialization_kind::forbidden);

static_assert(kind_of<resource_ref> == serialization_kind::visit, "opt-in must beat the inference");
static_assert(kind_of<secret_key> == serialization_kind::forbidden, "opt-in must beat the inference");
static_assert(kind_of<legacy_blob> == serialization_kind::blit, "specialization must beat everything");

// The ladder is ordered: a specialization of the *traits* wins over an opt-in
// member, which wins over the inference. Confirm the middle rung is really doing
// the work and not agreeing by luck.
static_assert(std::is_trivially_copyable_v<resource_ref>,
              "resource_ref would infer `blit`; the opt-in is what changes it");

// A trait is only useful if something branches on it. This is the shape that
// branch takes -- and note `forbidden` becomes a compile error at the point of
// use, not a runtime check.
template<typename Type>
    requires(kind_of<Type> != serialization_kind::forbidden)
[[nodiscard]] std::size_t write(const Type &value, std::vector<std::byte> &out) {
    if constexpr(kind_of<Type> == serialization_kind::blit) {
        const auto offset = out.size();
        out.resize(offset + sizeof(Type));
        std::memcpy(out.data() + offset, &value, sizeof(Type));
        return sizeof(Type);
    } else {
        return value.write_to(out);
    }
}

std::size_t write(const std::string &value, std::vector<std::byte> &out) {
    for(const char c: value) {
        out.push_back(static_cast<std::byte>(c));
    }
    return value.size();
}

template<typename Type>
concept serializable = requires(const Type &value, std::vector<std::byte> &out) { write(value, out); };

static_assert(serializable<pod>);
static_assert(serializable<std::string>);
static_assert(!serializable<secret_key>, "forbidden must be unrepresentable, not merely discouraged");
static_assert(!serializable<int *>);

} // namespace

int main() {
    acpp::testing::suite suite{"module 02 / serialization_traits"};

    std::vector<std::byte> out;
    suite.check(write(pod{1, 2.0f}, out) == sizeof(pod), "inferred blit path writes the object representation");
    suite.check(write(std::string{"abc"}, out) == 3u, "visit path writes through the type's own writer");
    suite.check(out.size() == sizeof(pod) + 3u, "both paths appended to the same stream");

    suite.check(kind_of<pod> == serialization_kind::blit, "level 1: inferred");
    suite.check(kind_of<resource_ref> == serialization_kind::visit, "level 2: inline opt-in");
    suite.check(kind_of<legacy_blob> == serialization_kind::blit, "level 3: specialization");

    // stable_layout is a derived trait: it composes the two questions rather
    // than making the user answer a third one.
    suite.check(acpp::serialization_traits<pod>::stable_layout, "derived trait composes");
    suite.check(!acpp::serialization_traits<std::string>::stable_layout, "derived trait composes");

    return suite.report();
}
