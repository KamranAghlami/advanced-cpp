// Module 6, exercise 1 -- swap_and_pop and the paged sparse array, with the
// invariant checked after randomized sequences.
//
// The invariant is one line:  packed[sparse[e]] == e  for every contained e.
// Everything a sparse set does has to preserve it, and almost every way of
// getting the implementation wrong breaks it.

#include <cstdint>
#include <random>
#include <unordered_set>
#include <vector>

#include <acpp/sparse_set.hpp>
#include <acpp/testing.hpp>

namespace {

enum class entity : std::uint32_t {};

using set_type = acpp::basic_sparse_set<entity>;
using traits = acpp::handle_traits<entity>;

[[nodiscard]] entity make(const std::uint32_t index, const std::uint16_t version = 0u) noexcept {
    return traits::construct(index, version);
}

// The invariant, spelled out. `index(e)` reads the sparse array; `set[pos]`
// reads the packed array; the two must agree for every element.
[[nodiscard]] bool holds(const set_type &set, const std::unordered_set<std::uint32_t> &expected) {
    if(set.count() != expected.size()) {
        return false;
    }

    for(const auto raw: expected) {
        const auto entt = make(raw);
        if(!set.contains(entt) || set[set.index(entt)] != entt) {
            return false;
        }
    }

    // And nothing else is in there. Checking only the forward direction would
    // pass for a set that never removes anything.
    for(std::size_t pos = 0u; pos < set.size(); ++pos) {
        if(!expected.contains(traits::to_index(set[pos]))) {
            return false;
        }
    }

    return true;
}

} // namespace

int main() {
    acpp::testing::suite suite{"module 06 / sparse_set_invariants"};

    {
        set_type set;

        suite.check(set.empty() && set.size() == 0u, "starts empty");
        suite.check(!set.contains(make(0u)), "contains nothing");

        set.push(make(3u));
        set.push(make(1u));
        set.push(make(42u));

        suite.check(set.size() == 3u, "three elements");
        suite.check(set.contains(make(3u)) && set.contains(make(42u)), "lookup works");
        suite.check(!set.contains(make(4u)), "and says no to absent ids");

        // Stale handles must not be found. This is Module 5's version check
        // doing its job inside the container.
        suite.check(!set.contains(make(3u, 1u)), "a stale version is not contained");

        suite.check(set.push(make(3u)) == set_type::max_size, "a duplicate push is refused");
        suite.check(set.size() == 3u, "and changes nothing");
    }

    // --- paging -------------------------------------------------------------
    //
    // The sparse array is indexed by entity *index*, which can be arbitrarily
    // spread out. A flat array sized to the largest id would waste memory
    // catastrophically; paging bounds the waste to one page.
    {
        set_type set;

        set.push(make(0u));
        set.push(make(1'000'000u)); // ~245 pages apart at 4096 per page

        suite.check(set.size() == 2u, "sparse ids do not need a dense sparse array");
        suite.check(set.contains(make(0u)) && set.contains(make(1'000'000u)), "both are found");
        suite.check(!set.contains(make(500'000u)), "and the untouched page in between reads as absent");
    }

    // --- the randomized sequence --------------------------------------------
    {
        std::mt19937 random{20260825u};
        set_type set;
        std::unordered_set<std::uint32_t> expected;

        bool invariant_held = true;
        std::size_t operations = 0u;

        for(int step = 0; step < 4000 && invariant_held; ++step) {
            // A small id space on purpose: collisions, re-inserts of previously
            // erased ids, and repeated reuse of packed slots are where the
            // bookkeeping goes wrong.
            const auto raw = static_cast<std::uint32_t>(random() % 300u);
            const auto entt = make(raw);

            if(expected.contains(raw)) {
                set.erase(entt);
                expected.erase(raw);
            } else {
                set.push(entt);
                expected.insert(raw);
            }

            ++operations;
            invariant_held = holds(set, expected);
        }

        suite.check(invariant_held, "packed[sparse[e]] == e held across 4000 randomized operations");
        suite.note("%zu operations, %zu live at the end", operations, set.count());
    }

    // --- what swap_and_pop actually does ------------------------------------
    //
    // The last element is moved into the hole. That keeps the array dense with
    // no holes, and it means the moved element's *position* changed -- which is
    // exactly what a pointer into the packed array would have been relying on.
    {
        set_type set;
        for(std::uint32_t i = 0u; i < 5u; ++i) {
            set.push(make(i));
        }

        const auto moved = set[4u];
        suite.check(set.index(moved) == 4u, "the last element is at position 4");

        set.erase(make(1u));

        suite.check(set.size() == 4u, "the array shrank");
        suite.check(set.index(moved) == 1u, "and the last element moved into the hole");
        suite.check(set[1u] == moved, "invariant preserved through the move");
        suite.check(set.free_list() == set_type::max_size, "swap_and_pop leaves no holes");
    }

    // --- erasing during backward iteration ----------------------------------
    //
    // The iterator starts at the high end and walks toward 0, so the element
    // swapped into a hole always comes from a slot already passed. Nothing is
    // skipped. Proved rather than asserted.
    {
        set_type set;
        for(std::uint32_t i = 0u; i < 50u; ++i) {
            set.push(make(i));
        }

        std::vector<std::uint32_t> visited;
        for(auto it = set.begin(); it != set.end();) {
            const auto entt = *it;
            visited.push_back(traits::to_index(entt));

            if(traits::to_index(entt) % 3u == 0u) {
                set.erase(entt);
                // No ++: the slot now holds a different element, which this
                // iterator has not yet visited... except it has, because the
                // replacement came from above. Re-reading is the bug the design
                // avoids; advancing is correct.
                ++it;
            } else {
                ++it;
            }
        }

        std::unordered_set<std::uint32_t> unique{visited.begin(), visited.end()};
        suite.check(visited.size() == 50u, "every element was visited exactly once");
        suite.check(unique.size() == 50u, "and none twice");
        suite.check(set.count() == 33u, "17 multiples of three were erased");
    }

    return suite.report();
}
