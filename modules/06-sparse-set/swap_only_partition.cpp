// Module 6, section 6.3 -- the third policy, and the one that explains entity
// recycling.
//
// swap_only never destroys anything. The packed array is partitioned at `head`:
// live below, released above. Releasing swaps the element across the boundary
// and bumps its version; allocating moves the boundary back down.
//
// So **the partition is the free list**. A registry's entity storage needs no
// separate recycling structure at all -- which is where Module 5's generational
// handles and Module 6's sparse set meet.

#include <cstdint>

#include <acpp/sparse_set.hpp>
#include <acpp/testing.hpp>

namespace {

enum class entity : std::uint32_t {};

using set_type = acpp::basic_sparse_set<entity>;
using traits = acpp::handle_traits<entity>;

[[nodiscard]] entity make(const std::uint32_t index, const std::uint16_t version = 0u) noexcept {
    return traits::construct(index, version);
}

} // namespace

int main() {
    acpp::testing::suite suite{"module 06 / swap_only_partition"};

    set_type set{acpp::deletion_policy::swap_only};

    // head starts at 0 for this policy and at max_size for the other two. The
    // branchless policy_to_head() is what produces both from one expression.
    suite.check(set.free_list() == 0u, "swap_only starts with an empty live partition");

    for(std::uint32_t i = 0u; i < 6u; ++i) {
        set.push(make(i));
    }

    suite.check(set.size() == 6u && set.count() == 6u, "six live");
    suite.check(set.free_list() == 6u, "the boundary is at the end");

    set.erase(make(2u));

    suite.check(set.size() == 6u, "nothing was destroyed -- the array is the same length");
    suite.check(set.count() == 5u, "but only five are live");
    suite.check(set.free_list() == 5u, "the boundary moved down by one");
    suite.check(!set.contains(make(2u)), "the released id is no longer contained");

    // The released element is still physically present, above the boundary,
    // with its version bumped -- but `contains` says no, because it checks the
    // partition as well as the version. "Present in the array" and "live" are
    // different questions, and only the second one is the container's answer.
    suite.check(!set.contains(make(2u, 1u)), "the next generation is not live either -- not yet");

    set.erase(make(4u));
    set.erase(make(0u));

    suite.check(set.count() == 3u && set.size() == 6u, "three live, six slots, nothing destroyed");

    // Recycling, and the claim the module is really making: bringing a released
    // slot back is one swap across the boundary. The array does not grow.
    const auto before = set.size();
    const auto recycled = set.push(make(2u, 1u));

    suite.check(set.size() == before, "recycling did not grow the packed array");
    suite.check(recycled < set.free_list(), "the recycled element is below the boundary");
    suite.check(set.contains(make(2u, 1u)), "and is live at its new generation");
    suite.check(!set.contains(make(2u)), "while the old generation stays dead");
    suite.check(set.count() == 4u, "four live again");

    // The stale generation must not be able to claim the slot back.
    suite.check(set.push(make(4u)) == set_type::max_size,
                "pushing a retired generation is refused, not silently resurrected");

    // Releasing an element that is already above the boundary is refused,
    // because contains() checks the partition as well as the version.
    suite.check(!set.erase(make(0u)), "a stale handle cannot release a slot twice");

    suite.note("size=%zu live=%zu boundary=%zu", set.size(), set.count(), set.free_list());

    return suite.report();
}
