#ifndef ACPP_MODULE01_ODR_SHARED_HPP
#define ACPP_MODULE01_ODR_SHARED_HPP

// Module 1, exercise 3 -- the two implementations under test.
//
// naive::type_index is what everyone writes first: a counter and a per-type
// cache, both function-local statics in a header. Correct inside one binary.
// Across a shared-object boundary it silently becomes two counters and two IDs
// for one type -- see NOTES.md for the full mechanism.

#include <acpp/config.hpp>
#include <acpp/type_info.hpp>

namespace odr {

// ACPP_EXPORT on the *types* is not decoration either. GCC computes a template
// instantiation's visibility as the minimum over the template and its arguments,
// so a hidden argument hides the instantiation no matter what the template says.
struct ACPP_EXPORT alpha {};
struct ACPP_EXPORT beta {};
struct ACPP_EXPORT gamma {};

// Touched by the plugin at load time, never by the host. Their only job is to
// advance the plugin's counter so a duplicated counter provably disagrees rather
// than coincidentally agreeing -- see the ordering note in odr_across_dso.cpp.
struct ACPP_EXPORT plugin_private_a {};
struct ACPP_EXPORT plugin_private_b {};

namespace naive {

struct ACPP_HIDDEN counter final {
    [[nodiscard]] static acpp::id_type next() noexcept {
        static acpp::id_type value{};
        return value++;
    }
};

template<typename Type>
struct ACPP_HIDDEN type_index final {
    [[nodiscard]] static acpp::id_type value() noexcept {
        static const acpp::id_type value = counter::next();
        return value;
    }
};

} // namespace naive

// extern "C" so the host names them without depending on the plugin's mangling,
// which is what a plugin ABI looks like in practice anyway.
extern "C" {

ACPP_EXPORT acpp::id_type plugin_naive_index_of_beta() noexcept;
ACPP_EXPORT acpp::id_type plugin_fixed_index_of_beta() noexcept;
ACPP_EXPORT acpp::id_type plugin_hash_of_beta() noexcept;

} // extern "C"

} // namespace odr

#endif // ACPP_MODULE01_ODR_SHARED_HPP
