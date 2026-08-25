// Module 3, exercise 1 -- compressed_pair from scratch, and the same thing with
// [[no_unique_address]], measured side by side.

#include <cstddef>
#include <memory>
#include <string>
#include <type_traits>

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

// --- the claim ---------------------------------------------------------------

// An empty half costs nothing. The pair is the size of the other half.
static_assert(sizeof(pair<empty, int>) == sizeof(int));
static_assert(sizeof(pair<int, empty>) == sizeof(int));
static_assert(sizeof(pair<std::allocator<int>, int *>) == sizeof(int *));

// Two *differently typed* empty halves cost one byte -- the minimum a complete
// object may occupy.
static_assert(sizeof(pair<empty, also_empty>) == 1u);

// Two halves of the SAME empty type cost two, and this is the result worth
// stopping on. The Tag parameter makes the code compile (see
// tagless_pair_fails.cpp), but it cannot make the two subobjects free: distinct
// objects of the same type must have distinct addresses, so the second base is
// pushed to offset 1. Compression is per *type*, not per empty member.
static_assert(sizeof(pair<empty, empty>) == 2u);

// Neither half empty: no compression, and no overhead either.
static_assert(sizeof(pair<stateful, int>) == 2u * sizeof(int));

// `final` is the trap. It is empty, so is_empty_v says yes -- but inheriting
// from it is ill-formed, so it cannot be compressed. is_ebco_eligible_v asks the
// question you actually meant.
static_assert(std::is_empty_v<final_empty>);
static_assert(!acpp::is_ebco_eligible_v<final_empty>);
static_assert(sizeof(pair<final_empty, int>) == 2u * sizeof(int));

// --- the C++20 spelling ------------------------------------------------------

static_assert(sizeof(nua<empty, int>) == sizeof(int));
static_assert(sizeof(nua<int, empty>) == sizeof(int));

// [[no_unique_address]] handles `final` that the inheritance version cannot --
// there is no base class involved, so finality is irrelevant.
static_assert(sizeof(nua<final_empty, int>) == sizeof(int));

// [[no_unique_address]] does not escape the distinct-address rule either: two
// members of the same empty type still cannot overlap.
static_assert(sizeof(nua<empty, empty>) == 2u);
static_assert(sizeof(nua<empty, also_empty>) == 1u);

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
    suite.check(sizeof(pair<empty, empty>) == 2u, "two halves of the SAME empty type cannot overlap");
    suite.check(sizeof(pair<final_empty, int>) == 2u * sizeof(int),
                "an empty *final* type cannot be compressed by inheritance");
    suite.check(sizeof(nua<final_empty, int>) == sizeof(int),
                "[[no_unique_address]] compresses it anyway");

    // Two independent pairs must not alias each other's compressed halves.
    pair<empty, int> a{empty{}, 1};
    pair<empty, int> b{empty{}, 2};
    a.second() = 10;
    suite.check(a.second() == 10 && b.second() == 2, "compressed halves are per-instance");

    // The address rule, which is what produces the sizes above. Two empty bases
    // of DIFFERENT types may legitimately share an address -- and do, which is
    // why that pair is one byte. Two of the SAME type may not, which is why
    // pair<empty, empty> is two.
    pair<empty, also_empty> mixed;
    pair<empty, empty> same;
    suite.check(static_cast<const void *>(&mixed.first()) == static_cast<const void *>(&mixed.second()),
                "empty bases of different types may share an address");
    suite.check(static_cast<const void *>(&same.get<0>()) != static_cast<const void *>(&same.get<1>()),
                "empty bases of the same type may not");

    // A pair of non-movable, non-copyable halves is still constructible.
    pair<std::string, std::string> pieces{
        std::piecewise_construct, std::forward_as_tuple(3u, 'a'), std::forward_as_tuple("bb")};
    suite.check(pieces.first() == "aaa" && pieces.second() == "bb", "piecewise construction");

    acpp::swap(a, b);
    suite.check(a.second() == 2 && b.second() == 10, "swap");

    return suite.report();
}
