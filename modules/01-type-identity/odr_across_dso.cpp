// Module 1, exercise 3 -- the host. Two TUs and one shared object.
//
// Ordering, which is the whole trick to making this deterministic:
//
//   plugin load  naive:  private_a=0 private_b=1        (plugin's own counter,
//                fixed:  private_a=0 private_b=1         or the shared one)
//   host         naive:  alpha=0 beta=1 gamma=2          (host's own counter)
//                fixed:  alpha=2 beta=3 gamma=4          (continues the shared one)
//   plugin asked naive:  beta=2  != host's 1             -> bug, provably
//                fixed:  beta=3  == host's 3             -> merged
//
// Without the two private types the naive answer would have been 1 on both
// sides, agreeing by accident. That is why this bug survives testing.

#include <acpp/testing.hpp>
#include <acpp/type_info.hpp>

#include "odr_shared.hpp"

int main() {
    acpp::testing::suite suite{"module 01 / odr_across_dso"};

    const auto host_naive_alpha = odr::naive::type_index<odr::alpha>::value();
    const auto host_naive_beta = odr::naive::type_index<odr::beta>::value();
    const auto host_fixed_alpha = acpp::type_index<odr::alpha>::value();
    const auto host_fixed_beta = acpp::type_index<odr::beta>::value();

    suite.check(host_naive_beta == host_naive_alpha + 1u && host_fixed_beta == host_fixed_alpha + 1u,
                "both implementations hand out consecutive ids in-process");

    const auto plugin_naive_beta = odr::plugin_naive_index_of_beta();
    const auto plugin_fixed_beta = odr::plugin_fixed_index_of_beta();

    suite.note("beta: naive host=%u plugin=%u | fixed host=%u plugin=%u",
               host_naive_beta, plugin_naive_beta, host_fixed_beta, plugin_fixed_beta);

#if defined __GNUC__ && !defined _WIN32
    // The bug. If this stops failing, the build stopped hiding symbols by
    // default and the exercise no longer demonstrates anything.
    suite.check(plugin_naive_beta != host_naive_beta,
                "BUG REPRODUCED: one type, two ids across the boundary");
#else
    suite.note("visibility semantics are ELF-specific; skipping the reproduction");
#endif

    // The fix: ACPP_API on the template, ACPP_EXPORT on the argument types.
    suite.check(plugin_fixed_beta == host_fixed_beta,
                "the id agrees across the boundary once both are visible");

    // And what should have crossed the boundary in the first place.
    suite.check(odr::plugin_hash_of_beta() == acpp::type_hash<odr::beta>::value(),
                "type_hash agrees with no linkage tricks at all");

    return suite.report();
}
