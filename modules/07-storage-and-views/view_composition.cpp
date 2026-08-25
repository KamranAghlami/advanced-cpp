// Module 7, exercises 2, 3 and 4 -- smallest-pool selection, the extended
// iterator, and the empty tag component.

#include <cstdint>
#include <tuple>
#include <type_traits>
#include <vector>

#include <acpp/testing.hpp>
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

// Empty: page_size 0, no payload array at all (Modules 2 and 3).
struct stationary {};

using position_storage = acpp::basic_storage<position, entity>;
using velocity_storage = acpp::basic_storage<velocity, entity>;
using stationary_storage = acpp::basic_storage<stationary, entity>;

static_assert(stationary_storage::page_size == 0u, "a tag allocates no payload pages");
static_assert(std::is_same_v<decltype(std::declval<stationary_storage &>().get_as_tuple(entity{})), std::tuple<>>,
              "and contributes an empty tuple, which tuple_cat simply drops");

} // namespace

int main() {
    acpp::testing::suite suite{"module 07 / view_composition"};

    // --- exercise 2: smallest-pool selection --------------------------------
    //
    // A view iterates one storage and filters against the rest. Lead with the
    // 100,000-element pool instead of the 100-element one and you do 100,000
    // lookups instead of 100.
    {
        position_storage positions;
        velocity_storage velocities;

        for(std::uint32_t i = 0u; i < 100'000u; ++i) {
            positions.emplace(make(i), position{static_cast<float>(i), 0.0f});
        }

        for(std::uint32_t i = 0u; i < 100u; ++i) {
            velocities.emplace(make(i * 7u), velocity{1.0f, 0.0f});
        }

        auto view = acpp::make_view(positions, velocities);

        suite.check(view.handle() == static_cast<const acpp::basic_sparse_set<entity> *>(&velocities),
                    "the view leads with the smaller pool");
        suite.check(view.size_hint() == 100u, "so size_hint is 100, not 100,000");

        std::size_t visited = 0u;
        for(const auto entt: view) {
            (void)entt;
            ++visited;
        }

        suite.check(visited == 100u, "and the iteration visits 100 entities");

        // The choice is not made once and forgotten -- a view holds pointers,
        // so "smallest" can go stale.
        for(std::uint32_t i = 0u; i < 200'000u; ++i) {
            velocities.emplace(make(1'000'000u + i), velocity{});
        }

        suite.check(view.handle() == static_cast<const acpp::basic_sparse_set<entity> *>(&velocities),
                    "before refresh(), the stale choice is still in force");

        view.refresh();

        suite.check(view.handle() == static_cast<const acpp::basic_sparse_set<entity> *>(&positions),
                    "after refresh(), the view leads with positions");
        suite.check(view.size_hint() == 100'000u, "which is now the smaller pool");
    }

    // --- exercise 3: the extended iterator ----------------------------------
    {
        position_storage positions;
        velocity_storage velocities;

        for(std::uint32_t i = 0u; i < 10u; ++i) {
            positions.emplace(make(i), position{static_cast<float>(i), 0.0f});
            if(i % 2 == 0u) {
                velocities.emplace(make(i), velocity{2.0f, 3.0f});
            }
        }

        auto view = acpp::make_view(positions, velocities);

        // Structured bindings over tuple_cat(entity, components...).
        std::size_t moved = 0u;
        for(auto [entt, pos, vel]: view.each()) {
            (void)entt;
            pos.x += vel.dx;
            pos.y += vel.dy;
            ++moved;
        }

        suite.check(moved == 5u, "each() visited the five entities in both pools");
        suite.check(positions.get(make(4u)).x == 6.0f, "and the writes went through the reference");
        suite.check(positions.get(make(3u)).x == 3.0f, "while a non-matching entity was untouched");

        // The bindings really are references into the storages, not copies.
        static_assert(std::is_lvalue_reference_v<std::tuple_element_t<1u, decltype(view.each())::iterator::value_type>>);
        static_assert(std::is_lvalue_reference_v<std::tuple_element_t<2u, decltype(view.each())::iterator::value_type>>);
    }

    // --- exercise 4: an empty tag composes ----------------------------------
    {
        position_storage positions;
        stationary_storage tags;

        for(std::uint32_t i = 0u; i < 8u; ++i) {
            positions.emplace(make(i), position{static_cast<float>(i), 0.0f});
            if(i % 4 == 0u) {
                tags.emplace(make(i));
            }
        }

        auto view = acpp::make_view(positions, tags);

        suite.check(view.size_hint() == 2u, "the tag pool is the smaller one and leads");

        // The tuple has TWO elements, not three: the tag contributes nothing.
        // No special case anywhere -- tuple_cat drops an empty tuple.
        std::size_t tagged = 0u;
        float total = 0.0f;
        for(auto [entt, pos]: view.each()) {
            (void)entt;
            total += pos.x;
            ++tagged;
        }

        suite.check(tagged == 2u, "two tagged entities");
        suite.check(total == 4.0f, "entities 0 and 4");
        static_assert(std::tuple_size_v<decltype(view.each())::iterator::value_type> == 2u,
                      "entity + position, and nothing for the tag");
    }

    // --- a single-component view compiles the filter away -------------------
    {
        position_storage positions;
        for(std::uint32_t i = 0u; i < 6u; ++i) {
            positions.emplace(make(i), position{static_cast<float>(i), 0.0f});
        }

        auto view = acpp::make_view(positions);
        std::size_t seen = 0u;
        for(auto [entt, pos]: view.each()) {
            (void)entt;
            (void)pos;
            ++seen;
        }

        suite.check(seen == 6u, "a one-pool view visits everything");
        suite.check(view.size_hint() == 6u, "with no filtering at all");
    }

    // --- exclusions ---------------------------------------------------------
    {
        position_storage positions;
        velocity_storage velocities;
        stationary_storage frozen;

        for(std::uint32_t i = 0u; i < 10u; ++i) {
            positions.emplace(make(i), position{static_cast<float>(i), 0.0f});
            velocities.emplace(make(i), velocity{});
        }

        frozen.emplace(make(2u));
        frozen.emplace(make(5u));

        acpp::basic_view<acpp::get_t<position_storage, velocity_storage>,
                         acpp::exclude_t<stationary_storage>>
            view{positions, velocities, frozen};

        std::size_t seen = 0u;
        bool excluded_absent = true;
        for(const auto entt: view) {
            excluded_absent = excluded_absent && (entt != make(2u)) && (entt != make(5u));
            ++seen;
        }

        suite.check(seen == 8u, "exclusions removed two entities");
        suite.check(excluded_absent, "and removed the right two");
    }

    return suite.report();
}
