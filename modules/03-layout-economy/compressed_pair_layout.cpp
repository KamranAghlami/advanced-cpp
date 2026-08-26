// Module 3, exercise 1 -- compressed_pair from scratch, and the same thing with
// [[no_unique_address]], measured side by side.

#include <cstddef>
#include <memory>
#include <string>
#include <type_traits>

#include <acpp/config.hpp>
#include <acpp/compressed_pair.hpp>
#include <acpp/testing.hpp>

namespace {

struct empty {};

struct also_empty {
    void method() const {} // still empty: member functions are not storage
};

struct final_empty final {};

struct stateful {
    int value;
};

template<typename First, typename Second>
using pair = acpp::compressed_pair<First, Second>;

template<typename First, typename Second>
using nua = acpp::nua_pair<First, Second>;

// [[no_unique_address]] is permission, not obligation, and MSVC declines it.
// acpp::nua_compresses records which way this toolchain went, with the
// measurement behind it; the expectations below are written in terms of it so
// MSVC keeps real assertions rather than skipped ones. If MSVC ever starts
// honouring the attribute, these fail.
using acpp::nua_compresses;

// --- the claim ---------------------------------------------------------------

// An empty half costs nothing. The pair is the size of the other half.
static_assert(sizeof(pair<empty, int>) == sizeof(int));
static_assert(sizeof(pair<int, empty>) == sizeof(int));
static_assert(sizeof(pair<std::allocator<int>, int *>) == sizeof(int *));

// Two *differently typed* empty halves cost one byte -- the minimum a complete
// object may occupy.
static_assert(sizeof(pair<empty, also_empty>) == 1u);

// Two halves of the SAME empty type: the Tag parameter makes this compile (see
// tagless_pair_fails.cpp) but cannot make it free. On the Itanium ABI the two
// `empty` base subobjects are the same type and so must have distinct
// addresses, which puts the second at offset 1 and the pair at 2 bytes.
//
// MSVC lays this out differently -- the exact number is reported at run time
// rather than asserted, because it is an ABI choice and not a language rule.
// What IS a language rule is the distinct-address requirement, and that is
// checked directly in main() where it can name the two addresses.
static_assert(sizeof(pair<empty, empty>) >= 1u);

// Neither half empty: no compression, and no overhead either.
static_assert(sizeof(pair<stateful, int>) == 2u * sizeof(int));

// `final` is the trap. It is empty, so is_empty_v says yes -- but inheriting
// from it is ill-formed, so it cannot be compressed. is_ebco_eligible_v asks the
// question you actually meant.
static_assert(std::is_empty_v<final_empty>);
static_assert(!acpp::is_ebco_eligible_v<final_empty>);
static_assert(sizeof(pair<final_empty, int>) == 2u * sizeof(int));

// --- the C++20 spelling ------------------------------------------------------

// An ignored attribute leaves the empty member occupying a byte, which then
// pads out to the other member's alignment: 1 + 3 + 4 rather than 4.
static_assert(sizeof(nua<empty, int>) == (nua_compresses ? sizeof(int) : 2u * sizeof(int)));
static_assert(sizeof(nua<int, empty>) == (nua_compresses ? sizeof(int) : 2u * sizeof(int)));

// [[no_unique_address]] handles `final`, which the inheritance version cannot --
// there is no base class involved, so finality is irrelevant. Where the
// attribute is honoured at all.
static_assert(sizeof(nua<final_empty, int>) == (nua_compresses ? sizeof(int) : 2u * sizeof(int)));

// [[no_unique_address]] does not escape the distinct-address rule either: two
// members of the same empty type still cannot overlap.
// Two members of the same empty type cannot overlap either way, so this one
// number is the same on every implementation -- for two different reasons.
static_assert(sizeof(nua<empty, empty>) == 2u);
static_assert(sizeof(nua<empty, also_empty>) == (nua_compresses ? 1u : 2u));

// --- behaviour, not just layout ----------------------------------------------

// The compressed element must still behave like a stored value: writable,
// addressable, and distinct per pair instance.
static_assert(std::is_default_constructible_v<pair<empty, int>>);
static_assert(std::is_copy_constructible_v<pair<std::string, int>>);
static_assert(!std::is_copy_constructible_v<pair<std::unique_ptr<int>, int>>);

constexpr int constexpr_roundtrip() {
    pair<empty, int> p{empty{}, 40};
    p.second() += 2;
    return p.second();
}

static_assert(constexpr_roundtrip() == 42);

// Structured bindings, via the tuple_size/tuple_element specializations.
constexpr int structured() {
    pair<int, int> p{1, 2};
    auto [first, second] = p;
    return first * 10 + second;
}

static_assert(structured() == 12);

} // namespace

int main() {
    acpp::testing::suite suite{"module 03 / compressed_pair_layout"};

    suite.note("sizeof(int)=%zu  pair<empty,int>=%zu  pair<stateful,int>=%zu  pair<empty,empty>=%zu",
               sizeof(int), sizeof(pair<empty, int>), sizeof(pair<stateful, int>), sizeof(pair<empty, empty>));
    suite.note("nua<empty,int>=%zu  nua<final_empty,int>=%zu  pair<final_empty,int>=%zu",
               sizeof(nua<empty, int>), sizeof(nua<final_empty, int>), sizeof(pair<final_empty, int>));

    suite.check(sizeof(pair<empty, int>) == sizeof(int), "an empty half is free");
    suite.check(sizeof(pair<empty, also_empty>) == 1u, "two differently-typed empty halves cost one byte");
    suite.note("sizeof(pair<empty,empty>) = %zu  (an ABI choice, not a language rule)",
               sizeof(pair<empty, empty>));
    suite.check(sizeof(pair<final_empty, int>) == 2u * sizeof(int),
                "an empty *final* type cannot be compressed by inheritance");
    suite.note("[[no_unique_address]] %s on this implementation: nua<empty,int> = %zu",
               nua_compresses ? "compresses" : "is IGNORED", sizeof(nua<empty, int>));
    suite.check(sizeof(nua<final_empty, int>) == (nua_compresses ? sizeof(int) : 2u * sizeof(int)),
                nua_compresses ? "[[no_unique_address]] compresses a final empty type"
                               : "MSVC ignores [[no_unique_address]] -- why the EBO version still ships");

    // Two independent pairs must not alias each other's compressed halves.
    pair<empty, int> a{empty{}, 1};
    pair<empty, int> b{empty{}, 2};
    a.second() = 10;
    suite.check(a.second() == 10 && b.second() == 2, "compressed halves are per-instance");

    // The address rule, which is what produces the sizes above -- and the place
    // this test previously overreached. Two empty bases of DIFFERENT types *may*
    // share an address. "May" is permission, not obligation, so an implementation
    // that declines is still conforming and asserting it was simply wrong. It is
    // reported instead.
    //
    // Two subobjects of the SAME type may not share an address. That one is a
    // language guarantee, so it stays a check, and it is the rule that makes
    // pair<empty, empty> two bytes under the Itanium ABI.
    pair<empty, also_empty> mixed;
    pair<empty, empty> same;
    const auto offset_of = [](const auto &pair_ref, const void *member) {
        return static_cast<std::size_t>(static_cast<const char *>(member)
                                        - reinterpret_cast<const char *>(&pair_ref));
    };
    suite.note("empty bases of different types %s an address here  (offsets %zu / %zu, sizeof %zu)",
               static_cast<const void *>(&mixed.first()) == static_cast<const void *>(&mixed.second())
                   ? "share" : "do NOT share",
               offset_of(mixed, &mixed.first()), offset_of(mixed, &mixed.second()), sizeof(mixed));
    suite.note("same-type empty bases sit at offsets %zu / %zu of a %zu-byte pair",
               offset_of(same, &same.get<0>()), offset_of(same, &same.get<1>()), sizeof(same));
    suite.check(static_cast<const void *>(&same.get<0>()) != static_cast<const void *>(&same.get<1>()),
                "empty bases of the same type may not share an address");

    // A pair of non-movable, non-copyable halves is still constructible.
    pair<std::string, std::string> pieces{
        std::piecewise_construct, std::forward_as_tuple(3u, 'a'), std::forward_as_tuple("bb")};
    suite.check(pieces.first() == "aaa" && pieces.second() == "bb", "piecewise construction");

    acpp::swap(a, b);
    suite.check(a.second() == 2 && b.second() == 10, "swap");

    return suite.report();
}
