// Module 2, exercise 2 -- type_list_unique, twice, checked against EnTT's.
//
// This is one of the few exercises where linking EnTT is the point: the course
// asks to compare, and "same answer" is only meaningful against the real thing.
// The cost comparison lives next door in type_list_depth_probe.cpp.

#include <string>
#include <type_traits>
#include <vector>

#include <entt/core/type_traits.hpp>

#include <acpp/testing.hpp>
#include <acpp/type_traits.hpp>

namespace {

using acpp::type_list;

struct a {};
struct b {};
struct c {};

// --- correctness -----------------------------------------------------------

using duplicated = type_list<a, b, a, c, b, a, int, c, int>;
using expected = type_list<a, b, c, int>;

static_assert(std::is_same_v<acpp::type_list_unique_recursive_t<duplicated>, expected>);
static_assert(std::is_same_v<acpp::type_list_unique_folded_t<duplicated>, expected>);

// Order is part of the contract: keep the first occurrence. A "unique" that
// silently reorders would break any code that relies on a view's leading pool
// being the one it was written first.
static_assert(std::is_same_v<acpp::type_list_unique_t<type_list<c, b, a, c>>, type_list<c, b, a>>);

// Edge cases both implementations have to survive.
static_assert(std::is_same_v<acpp::type_list_unique_t<type_list<>>, type_list<>>);
static_assert(std::is_same_v<acpp::type_list_unique_t<type_list<a>>, type_list<a>>);
static_assert(std::is_same_v<acpp::type_list_unique_t<type_list<a, a, a, a>>, type_list<a>>);

// cv- and ref-qualified types are distinct types, and must stay distinct.
static_assert(acpp::type_list_unique_t<type_list<int, const int, int &, int &&>>::size == 4u);

// --- agreement with EnTT ---------------------------------------------------
//
// The lists are different types, so compare element-wise by converting ours into
// entt::type_list and asking EnTT whether the two agree.

template<typename>
struct as_entt;

template<typename... Type>
struct as_entt<type_list<Type...>> {
    using type = entt::type_list<Type...>;
};

template<typename List>
using as_entt_t = as_entt<List>::type;

template<typename List>
inline constexpr bool agrees_with_entt =
    std::is_same_v<as_entt_t<acpp::type_list_unique_t<List>>, entt::type_list_unique_t<as_entt_t<List>>>;

static_assert(agrees_with_entt<duplicated>);
static_assert(agrees_with_entt<type_list<>>);
static_assert(agrees_with_entt<type_list<a, a, a>>);
static_assert(agrees_with_entt<type_list<int, const int, int &>>);
static_assert(agrees_with_entt<type_list<std::string, std::vector<int>, std::string, double>>);

// --- the rest of the algebra ------------------------------------------------

static_assert(std::is_same_v<acpp::type_list_cat_t<type_list<a>, type_list<b, c>>, type_list<a, b, c>>);
static_assert(std::is_same_v<acpp::type_list_cat_t<>, type_list<>>);
static_assert(acpp::type_list_contains_v<type_list<a, b>, b>);
static_assert(!acpp::type_list_contains_v<type_list<a, b>, c>);
static_assert(!acpp::type_list_contains_v<type_list<>, a>);
static_assert(std::is_same_v<acpp::type_list_diff_t<type_list<a, b, c>, type_list<b>>, type_list<a, c>>);

// --- the vocabulary from 2.3 ------------------------------------------------

struct empty_base {};
struct final_empty final {};

static_assert(acpp::is_ebco_eligible_v<empty_base>);
static_assert(!acpp::is_ebco_eligible_v<final_empty>, "empty but final: inheriting from it is ill-formed");
static_assert(!acpp::is_ebco_eligible_v<int>);

static_assert(std::is_same_v<acpp::constness_as_t<int, const double>, const int>);
static_assert(std::is_same_v<acpp::constness_as_t<const int, double>, int>);

struct owner {
    int field;
    void method();
    int const_method() const noexcept;
};

static_assert(std::is_same_v<acpp::member_class_t<decltype(&owner::field)>, owner>);
static_assert(std::is_same_v<acpp::member_class_t<decltype(&owner::method)>, owner>);
static_assert(std::is_same_v<acpp::member_class_t<decltype(&owner::const_method)>, owner>);

} // namespace

int main() {
    acpp::testing::suite suite{"module 02 / type_list_algebra"};

    // Everything above is a static_assert, so reaching main is most of the
    // result. These restate the load-bearing ones at runtime so a failure is
    // reported rather than merely refusing to compile.
    suite.check(std::is_same_v<acpp::type_list_unique_recursive_t<duplicated>, expected>, "recursive unique");
    suite.check(std::is_same_v<acpp::type_list_unique_folded_t<duplicated>, expected>, "folded unique");
    suite.check(agrees_with_entt<duplicated>, "both agree with entt::type_list_unique_t");
    suite.check(acpp::type_list_unique_t<duplicated>::size == 4u, "9 types in, 4 out");

    return suite.report();
}
