// Module 6, exercise 2 -- in_place deletion and the intrusive free list.
//
// The encoding, which has to be exactly right:
//
//   packed[pos] = combine(previous_head, tombstone)
//                         ^^^^^^^^^^^^^  ^^^^^^^^^
//                         entity bits    version bits
//
// A freed slot stores the index of the *previously* freed slot in its entity
// bits and the tombstone version in its version bits. `head` points at the most
// recent hole. So the free list costs no extra memory -- it lives inside the
// array it manages -- and the tombstoned version keeps holes distinguishable
// from live entries during iteration.

#include <cstdint>
#include <vector>

#include <acpp/sparse_set.hpp>
#include <acpp/testing.hpp>

namespace {

enum class entity : std::uint32_t {};

using set_type = acpp::basic_sparse_set<entity>;
using traits = acpp::handle_traits<entity>;

[[nodiscard]] entity make(const std::uint32_t index) noexcept {
    return traits::construct(index, 0u);
}

// Walk the free list by hand, exactly as the container does, so the test is
// checking the encoding and not just the container's own accessor.
[[nodiscard]] std::vector<std::size_t> free_chain(const set_type &set) {
    std::vector<std::size_t> chain;

    for(auto cursor = set.free_list(); cursor != set_type::max_size;) {
        chain.push_back(cursor);
        cursor = static_cast<std::size_t>(traits::to_index(set[cursor]));
    }

    return chain;
}

} // namespace

int main() {
    acpp::testing::suite suite{"module 06 / in_place_free_list"};

    set_type set{acpp::deletion_policy::in_place};

    for(std::uint32_t i = 0u; i < 9u; ++i) {
        set.push(make(i));
    }

    suite.check(set.size() == 9u && set.count() == 9u, "nine elements, no holes");
    suite.check(set.free_list() == set_type::max_size, "the free list starts empty");

    // Erase every third element, in ascending order, so the LIFO claim below
    // has a direction to be wrong in.
    set.erase(make(0u));
    set.erase(make(3u));
    set.erase(make(6u));

    suite.check(set.size() == 9u, "the packed array did NOT shrink -- holes stay in place");
    suite.check(set.count() == 6u, "six live elements");

    // The positions of the survivors are unchanged. That is the entire reason
    // this policy exists: a pointer into the parallel payload array stays valid.
    suite.check(set.index(make(8u)) == 8u, "element 8 did not move");
    suite.check(set.index(make(1u)) == 1u, "element 1 did not move");

    // The encoding, checked directly.
    suite.check(set.is_tombstone(set[0u]) && set.is_tombstone(set[3u]) && set.is_tombstone(set[6u]),
                "the vacated slots read as tombstones");
    suite.check(!set.is_tombstone(set[1u]), "and live slots do not");

    const auto chain = free_chain(set);
    suite.note("free list: head=%zu chain=[%zu, %zu, %zu]",
               set.free_list(), chain[0], chain[1], chain[2]);

    suite.check(chain.size() == 3u, "the free list has three entries");
    suite.check(chain[0] == 6u && chain[1] == 3u && chain[2] == 0u,
                "and threads most-recently-freed first, through the packed array itself");

    // --- LIFO reuse ---------------------------------------------------------
    //
    // The exercise's specific claim: reinserting must reuse holes in LIFO order.
    suite.check(set.push(make(20u)) == 6u, "the first reinsert takes the most recent hole");
    suite.check(set.push(make(21u)) == 3u, "then the next one down");
    suite.check(set.push(make(22u)) == 0u, "then the oldest");

    suite.check(set.size() == 9u, "and the array never grew");
    suite.check(set.free_list() == set_type::max_size, "the free list is empty again");
    suite.check(set.count() == 9u, "nine live elements");

    // The invariant still holds through all of that.
    bool invariant = true;
    for(const std::uint32_t raw: {1u, 2u, 4u, 5u, 7u, 8u, 20u, 21u, 22u}) {
        const auto entt = make(raw);
        invariant = invariant && set.contains(entt) && set[set.index(entt)] == entt;
    }

    suite.check(invariant, "packed[sparse[e]] == e survived hole reuse");

    // Only now does the array grow.
    suite.check(set.push(make(23u)) == 9u, "with no holes left, a push appends");
    suite.check(set.size() == 10u, "and the array grows by one");

    // --- iteration has to skip holes ----------------------------------------
    {
        set_type holed{acpp::deletion_policy::in_place};
        for(std::uint32_t i = 0u; i < 10u; ++i) {
            holed.push(make(i));
        }
        holed.erase(make(2u));
        holed.erase(make(5u));

        std::size_t live = 0u;
        std::size_t tombstones = 0u;
        for(const auto entt: holed) {
            if(holed.is_tombstone(entt)) {
                ++tombstones;
            } else {
                ++live;
            }
        }

        suite.check(live == 8u && tombstones == 2u,
                    "iteration walks holes too -- the caller pays the in_place tax");
    }

    // --- compact() pays it back --------------------------------------------
    {
        set_type holed{acpp::deletion_policy::in_place};
        for(std::uint32_t i = 0u; i < 10u; ++i) {
            holed.push(make(i));
        }
        holed.erase(make(2u));
        holed.erase(make(5u));
        holed.erase(make(9u)); // a hole at the very end, which compact must trim

        holed.compact();

        suite.check(holed.size() == 7u, "compact removed the holes");
        suite.check(holed.free_list() == set_type::max_size, "and emptied the free list");

        bool all_present = true;
        for(const std::uint32_t raw: {0u, 1u, 3u, 4u, 6u, 7u, 8u}) {
            const auto entt = make(raw);
            all_present = all_present && holed.contains(entt) && holed[holed.index(entt)] == entt;
        }

        suite.check(all_present, "every survivor is still findable at its new position");
    }

    return suite.report();
}
