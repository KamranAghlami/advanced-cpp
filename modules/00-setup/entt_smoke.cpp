// Module 0 -- EnTT smoke test.
//
// Proves the pinned EnTT checkout compiles, links and runs here, and touches the
// surface Module 1 opens with: type identity without RTTI, and the compile-time
// hash. Then does a registry/view roundtrip so the storage and iteration
// machinery of Modules 5-7 is known-good before we start reimplementing it.

#include <iostream>
#include <string_view>
#include <vector>

#include <entt/config/version.h>
#include <entt/core/hashed_string.hpp>
#include <entt/core/type_info.hpp>
#include <entt/entity/registry.hpp>

namespace {

struct position {
    float x, y;
};

struct velocity {
    float dx, dy;
};

// Empty type: component_traits::page_size == 0, so no payload is allocated at
// all (Modules 2 and 3). It still has to compose in a view -- see below.
struct stationary {};

int failures = 0;

void check(const bool ok, const std::string_view what) {
    std::cout << (ok ? "  ok    " : "  FAIL  ") << what << '\n';
    failures += static_cast<int>(!ok);
}

} // namespace

int main() {
    using namespace entt::literals;

    std::cout << "EnTT " << ENTT_VERSION_MAJOR << '.' << ENTT_VERSION_MINOR << '.'
              << ENTT_VERSION_PATCH << " (pinned 85c6bba)\n";

    // Module 1.1: the name is sliced out of __PRETTY_FUNCTION__, not typeid.
    // Deliberately `const` and not `constexpr`: whether this call resolves to the
    // ENTT_CONSTEVAL overload or the runtime fallback is exactly what Module 1.2
    // is about, and forcing constexpr here would paper over the answer.
    const std::string_view name = entt::type_name<std::vector<int>>::value();
    std::cout << "  type_name<std::vector<int>>() = " << name << '\n';
    check(name.find("vector") != std::string_view::npos, "type_name<> extracts a real name");

    // Module 1.4: FNV-1a folded at compile time. The static_assert is the proof --
    // it cannot pass unless both hashes were constant-evaluated.
    static_assert("POWER_ON"_hs != "POWER_OFF"_hs);
    check(entt::hashed_string{"POWER_ON"}.value() == "POWER_ON"_hs,
          "hashed_string agrees with the _hs literal");

    // Modules 5-7: generational handles, sparse-set storage, multi-pool views.
    entt::registry registry;

    for (int i = 0; i < 6; ++i) {
        const auto entity = registry.create();
        registry.emplace<position>(entity, static_cast<float>(i), 0.0f);

        if (i % 2 == 0) {
            registry.emplace<velocity>(entity, 1.0f, 0.0f);
        } else {
            registry.emplace<stationary>(entity);
        }
    }

    int moved = 0;

    for (auto [entity, pos, vel] : registry.view<position, velocity>().each()) {
        pos.x += vel.dx;
        moved += static_cast<int>(registry.valid(entity));
    }

    check(moved == 3, "view<position, velocity>().each() visited the 3 moving entities");

    // The empty component still composes in a view even with no payload array.
    check(registry.view<position, stationary>().size_hint() == 3,
          "empty component participates in a view");

    // Module 5: bumping the version must make the old handle fail validation.
    const auto doomed = registry.create();
    registry.destroy(doomed);
    check(!registry.valid(doomed), "stale handle fails validation after destroy");

    std::cout << (failures == 0 ? "entt_smoke: PASS\n" : "entt_smoke: FAIL\n");
    return failures == 0 ? 0 : 1;
}
