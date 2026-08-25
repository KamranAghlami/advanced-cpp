// Module 5, exercise 2 -- what happens at the 4,096th reuse of a slot.
//
// There is no universally right answer; the deliverable is a decision. All three
// policies are implemented so the trade is visible rather than argued, and
// NOTES.md records which one this project defaults to and why.
//
//   recycle  wrap and keep going. No leak. Detection is best-effort past 2^V.
//   retire   never reuse the slot. Detection stays exact. Leaks one index.
//   trap     stop. For when neither of the above is acceptable.

#include <cstdint>

#include <acpp/handle.hpp>
#include <acpp/testing.hpp>

namespace {

using acpp::exhaustion_policy;

// A 4/4 split so exhaustion is reachable in a test rather than in production.
template<exhaustion_policy Policy>
using allocator = acpp::handle_allocator<std::uint16_t, 4u, 4u, Policy>;

// EnTT's behaviour, reproduced: the successor skips the all-ones version,
// because that value belongs to the tombstone.
using traits = allocator<exhaustion_policy::recycle>::traits_type;

static_assert(traits::to_version(traits::next(traits::construct(0u, 13u))) == 14u);
static_assert(traits::to_version(traits::next(traits::construct(0u, 14u))) == 0u,
              "15 is reserved for the tombstone, so 14 wraps straight to 0");

template<exhaustion_policy Policy>
[[nodiscard]] std::size_t churn_one_slot(allocator<Policy> &handles, const std::size_t rounds) {
    std::size_t completed = 0u;

    for(std::size_t i = 0u; i < rounds; ++i) {
        const auto handle = handles.allocate();
        if(allocator<Policy>::is_null(handle)) {
            break;
        }
        if(!handles.release(handle)) {
            break;
        }
        ++completed;
    }

    return completed;
}

} // namespace

int main() {
    acpp::testing::suite suite{"module 05 / version_wraparound"};

    suite.note("split is 4/4: %u slots, %u usable versions (15 is the tombstone)",
               traits::index_mask, traits::version_mask);

    // --- recycle -------------------------------------------------------------
    {
        allocator<exhaustion_policy::recycle> handles;

        using recycler = allocator<exhaustion_policy::recycle>;

        const auto original = handles.allocate(); // version 0
        (void)handles.release(original);          // version 1

        // A lap is version_mask long, not version_mask + 1: the all-ones value
        // is reserved for the tombstone and next() steps over it. One release
        // has already happened, so version_mask - 1 more closes the lap.
        const auto rounds = churn_one_slot(handles, recycler::max_version - 1u);
        const auto after = handles.allocate();

        suite.note("recycle: %zu further round trips, then version %u (was %u)",
                   rounds, recycler::version_of(after), recycler::version_of(original));

        suite.check(recycler::index_of(after) == recycler::index_of(original), "the slot keeps being reused");
        suite.check(handles.size() == 1u, "no slot is leaked");

        // The honest statement of the cost: after a full lap, a handle from the
        // previous lap validates again. That is not a bug in the allocator; it
        // is the arithmetic of a finite version field, and the only defence is
        // choosing enough bits.
        suite.check(handles.is_valid(after), "the current handle is valid");
        suite.check(recycler::version_of(after) == recycler::version_of(original),
                    "a full lap returns to the original version -- detection has lapsed");
    }

    // --- retire --------------------------------------------------------------
    {
        allocator<exhaustion_policy::retire> handles;

        const auto original = handles.allocate();
        (void)handles.release(original);
        const auto rounds = churn_one_slot(handles, 20u);

        suite.note("retire: %zu round trips before the slot was retired; %zu retired, %zu slots",
                   rounds, handles.retirements(), handles.size());

        suite.check(handles.retirements() == 1u, "the exhausted slot was retired");

        // A retired slot is never handed out again, so the next allocate() must
        // come from a fresh index.
        using retirer = allocator<exhaustion_policy::retire>;
        const auto next = handles.allocate();
        suite.check(retirer::index_of(next) != retirer::index_of(original), "the retired slot is not reused");
        suite.check(!handles.is_valid(original), "and no handle to it ever validates again");
    }

    return suite.report();
}
