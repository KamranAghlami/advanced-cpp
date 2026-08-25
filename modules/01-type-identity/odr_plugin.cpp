// Module 1, exercise 3 -- the shared object.

#include "odr_shared.hpp"

namespace odr {
namespace {

// A plugin registering its own types at load time is the normal case, and it is
// what desynchronises a duplicated counter from the host's.
const struct warm_up {
    warm_up() noexcept {
        (void)naive::type_index<plugin_private_a>::value();
        (void)naive::type_index<plugin_private_b>::value();
        (void)acpp::type_index<plugin_private_a>::value();
        (void)acpp::type_index<plugin_private_b>::value();
    }
} warm_up_instance;

} // namespace

extern "C" {

acpp::id_type plugin_naive_index_of_beta() noexcept {
    return naive::type_index<beta>::value();
}

acpp::id_type plugin_fixed_index_of_beta() noexcept {
    return acpp::type_index<beta>::value();
}

// The control: type_hash needs no visibility attribute and no shared counter,
// because it is a pure function of the type's spelling. Both sides compute it
// independently and agree by construction.
acpp::id_type plugin_hash_of_beta() noexcept {
    return acpp::type_hash<beta>::value();
}

} // extern "C"

} // namespace odr
