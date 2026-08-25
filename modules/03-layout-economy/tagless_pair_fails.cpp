// Module 3, checkpoint -- "give the exact code that breaks without the Tag".
//
// This is that code. It must NOT compile, and the build asserts that it does not
// and that the diagnostic is the expected one (compile_fail_tagless_pair in this
// directory's CMakeLists.txt).
//
// Same structure as acpp::compressed_pair, with the disambiguating `size_t Tag`
// parameter removed. compressed_pair<empty, empty> then derives from
// compressed_pair_element<empty> twice, and a class may not name the same type
// twice as a direct base.

#include <type_traits>

namespace {

struct empty {};

// Primary: store by value.
template<typename Type>
struct tagless_element {
    [[nodiscard]] constexpr Type &get() noexcept { return value; }
    Type value;
};

// EBO specialization: inherit instead. Note there is no Tag.
template<typename Type>
    requires(std::is_empty_v<Type> && !std::is_final_v<Type>)
struct tagless_element<Type>: Type {
    [[nodiscard]] constexpr Type &get() noexcept { return *this; }
};

template<typename First, typename Second>
struct tagless_pair: tagless_element<First>, tagless_element<Second> {};

// Fine: tagless_element<empty> and tagless_element<int> are different types.
using ok = tagless_pair<empty, int>;
static_assert(sizeof(ok) == sizeof(int));

// The error: both bases are tagless_element<empty>.
using broken = tagless_pair<empty, empty>;

broken instance{};

} // namespace
