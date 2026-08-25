// Module 5 -- a split that does not fit must be rejected at compile time.
//
// 20 + 16 bits does not fit in a uint32_t. Without the static_assert in
// bit_split, index_mask and version_mask would silently overlap and every
// handle would alias another one -- the worst possible failure for a type whose
// entire job is to be unambiguous.
//
// Required to fail by compile_fail_oversized_split.

#include <cstdint>

#include <acpp/handle.hpp>

using broken = acpp::bit_split<std::uint32_t, 20u, 16u>;

broken::entity_type instance = broken::index_mask;
