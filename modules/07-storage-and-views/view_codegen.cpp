// Module 7, exercise 3 (codegen half) -- is the tuple machinery actually free?
//
// `each()` builds a tuple per element via an immediately-invoked generic lambda
// over an index_sequence, and structured bindings unpack it. That is a lot of
// apparatus for what should be two pointer increments and an add.
//
// Checked by codegen_view_* in this directory's CMakeLists.txt.

#include <cstdint>

#include <acpp/view.hpp>

namespace {

enum class entity : std::uint32_t {};

struct position {
    float x, y;
};

struct velocity {
    float dx, dy;
};

using position_storage = acpp::basic_storage<position, entity>;
using velocity_storage = acpp::basic_storage<velocity, entity>;

} // namespace

extern "C" {

// A single-pool each(): no filtering survives (Get == 1u, Exclude == 0u are
// compile-time constants), so this should be a plain strided loop over the
// paged payload -- no tuple construction, no std::get, no lambda call.
float acpp_probe_single_pool_sum(position_storage &positions) {
    float total = 0.0f;

    for(auto [entt, pos]: positions.each()) {
        (void)entt;
        total += pos.x;
    }

    return total;
}

// Paged access: pos / Page and pos % Page per element. Page is a compile-time
// power of two, so both must be a shift and an AND. If a `div` or `idiv`
// instruction appears here, the claim in storage.hpp is wrong.
float acpp_probe_paged_access(position_storage &positions, unsigned index) {
    return positions.begin()[static_cast<long>(index)].x;
}

// The two-pool case, where the filter chain does survive.
float acpp_probe_two_pool_sum(position_storage &positions, velocity_storage &velocities) {
    auto view = acpp::make_view(positions, velocities);
    float total = 0.0f;

    for(auto [entt, pos, vel]: view.each()) {
        (void)entt;
        total += pos.x * vel.dx;
    }

    return total;
}

} // extern "C"
