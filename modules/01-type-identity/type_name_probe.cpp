// Module 1, exercise 1 -- reproduce stripped_type_name from scratch.
//
// The implementation lives in src/acpp/type_info.hpp so later modules can use
// it. This is the proof that it works, plus the two questions the checkpoint
// asks: which overload gets picked, and what the `auto =` parameter is doing.
//
// The codegen half of the exercise ("zero runtime instructions at -O2") is
// type_name_codegen.cpp, checked by the asm probe in this directory's
// CMakeLists.txt -- there is no Compiler Explorer in CI, so the check runs here.

#include <map>
#include <string>
#include <string_view>
#include <vector>

#include <acpp/testing.hpp>
#include <acpp/type_info.hpp>

namespace {

struct plain_struct {};

namespace nested {
struct deeper {};
} // namespace nested

template<typename, typename>
struct two_params {};

// ---------------------------------------------------------------------------
// The checkpoint question, answered in code.
//
// `constexpr auto x = f()` only compiles if f() is a constant expression, and
// consteval functions are ONLY callable in one. So a successful static_assert on
// type_name<T>::value() is proof the consteval overload was selected -- there is
// no way for the runtime fallback to satisfy it.
// ---------------------------------------------------------------------------
static_assert(acpp::type_name<int>::value() == "int");
static_assert(acpp::type_name<plain_struct>::value().find("plain_struct") != std::string_view::npos);
static_assert(acpp::type_hash<int>::value() != acpp::type_hash<unsigned>::value());

// Same for the hash: a case label must be a constant expression.
constexpr int discriminate(const acpp::id_type hash) noexcept {
    switch(hash) {
    case acpp::type_hash<int>::value():
        return 1;
    case acpp::type_hash<double>::value():
        return 2;
    default:
        return 0;
    }
}

static_assert(discriminate(acpp::type_hash<double>::value()) == 2);

// The extraction has to survive a spelling that contains the suffix character.
// This is why stripped_type_name cuts at find_LAST_of(']') and not find_first_of:
// GCC spells this one `int [4]`, so the first ']' is inside the type name.
static_assert(acpp::type_name<int[4]>::value().find("int") != std::string_view::npos);
static_assert(acpp::type_name<int[4]>::value().find('[') != std::string_view::npos);

template<typename Type>
[[nodiscard]] bool names_itself(const std::string_view needle) {
    return acpp::type_name<Type>::value().find(needle) != std::string_view::npos;
}

} // namespace

int main() {
    acpp::testing::suite suite{"module 01 / type_name_probe"};

    suite.note("compiler spells std::vector<int> as: %.*s",
               static_cast<int>(acpp::type_name<std::vector<int>>::value().size()),
               acpp::type_name<std::vector<int>>::value().data());

    suite.check(names_itself<int>("int"), "fundamental type");
    suite.check(names_itself<std::vector<int>>("vector"), "class template");
    suite.check(names_itself<nested::deeper>("deeper"), "nested class");
    suite.check(names_itself<two_params<int, double>>("two_params"), "multi-argument template");
    suite.check(names_itself<std::map<std::string, std::vector<int>>>("map"), "deeply nested template");

    // No leading or trailing whitespace: find_first_not_of(' ', ...) in the
    // extraction is what buys this, and it is easy to get wrong.
    const auto name = acpp::type_name<std::vector<int>>::value();
    suite.check(!name.empty() && name.front() != ' ' && name.back() != ' ', "name is trimmed");

    // Identity is per-type, not per-spelling: cv and reference qualifiers are
    // part of the *type* here, so these are legitimately different names. That
    // is why type_id<T>() strips them before asking.
    suite.check(acpp::type_hash<int>::value() != acpp::type_hash<const int>::value(),
                "const int and int hash differently (they are different types)");
    suite.check(acpp::type_id<int>() == acpp::type_id<const int &>(),
                "type_id strips cv-ref before asking");

    // Sequential IDs are dense and first-touch ordered.
    const auto first = acpp::type_index<plain_struct>::value();
    const auto second = acpp::type_index<nested::deeper>::value();
    suite.check(second == first + 1u, "type_index hands out consecutive ids");
    suite.check(acpp::type_index<plain_struct>::value() == first, "type_index is stable within a run");

    return suite.report();
}
