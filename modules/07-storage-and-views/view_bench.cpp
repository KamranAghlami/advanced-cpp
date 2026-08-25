// Module 7, exercise 2 -- smallest-pool selection, measured.
//
// Two pools, 1,000,000 and 100 elements. The view picks the small one; the naive
// version always leads with pool A. Not a ctest entry: it is a measurement.

#include <chrono>
#include <cstdint>
#include <cstdio>

#include <acpp/view.hpp>

namespace {

enum class entity : std::uint32_t {};

using traits = acpp::handle_traits<entity>;

[[nodiscard]] entity make(const std::uint32_t index) noexcept {
    return traits::construct(index, 0u);
}

struct position {
    float x, y;
};

struct velocity {
    float dx, dy;
};

volatile float sink = 0.0f;

template<typename Fn>
[[nodiscard]] double time_ms(Fn &&fn) {
    const auto started = std::chrono::steady_clock::now();
    fn();
    const auto finished = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>{finished - started}.count();
}

} // namespace

int main() {
    constexpr std::uint32_t big = 1'000'000u;
    constexpr std::uint32_t small = 100u;

    acpp::basic_storage<position, entity> positions;
    acpp::basic_storage<velocity, entity> velocities;

    for(std::uint32_t i = 0u; i < big; ++i) {
        positions.emplace(make(i), position{static_cast<float>(i), 0.0f});
    }

    // Spread across the whole id range, so leading with the big pool cannot get
    // lucky with locality.
    for(std::uint32_t i = 0u; i < small; ++i) {
        velocities.emplace(make(i * (big / small)), velocity{1.0f, 1.0f});
    }

    auto view = acpp::make_view(positions, velocities);

    std::printf("pools: positions=%u  velocities=%u\n", big, small);
    std::printf("view leads with the %s pool (size_hint = %zu)\n",
                view.size_hint() == small ? "SMALL" : "BIG", view.size_hint());

    const auto smart_ms = time_ms([&] {
        float total = 0.0f;
        for(auto [entt, pos, vel]: view.each()) {
            (void)entt;
            total += pos.x * vel.dx;
        }
        sink = total;
    });

    // The naive version: iterate pool A and probe pool B. Exactly what the view
    // would do if unchecked_refresh() did not exist.
    const auto naive_ms = time_ms([&] {
        float total = 0.0f;
        for(const auto entt: static_cast<const acpp::basic_sparse_set<entity> &>(positions)) {
            if(velocities.contains(entt)) {
                total += positions.get(entt).x * velocities.get(entt).dx;
            }
        }
        sink = total;
    });

    std::printf("%-28s %8.3f ms\n", "smallest-pool first", smart_ms);
    std::printf("%-28s %8.3f ms\n", "always lead with positions", naive_ms);
    std::printf("%-28s %8.1fx\n", "ratio", naive_ms / smart_ms);

    return 0;
}
