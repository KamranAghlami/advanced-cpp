// Module 5, exercise 3 -- the property test.
//
// One property, stated once: *no stale handle ever validates.* Everything else
// here exists to generate situations where it might.
//
// Not a fuzzer, but the same shape: a deterministic PRNG so a failure is
// reproducible from the seed, a shadow model to compare against, and randomized
// operation sequences rather than a scripted scenario. Seeds are swept so a
// single unlucky run cannot pass by accident.

#include <cstdint>
#include <random>
#include <unordered_map>
#include <vector>

#include <acpp/handle.hpp>
#include <acpp/testing.hpp>

namespace {

enum class thing_id : std::uint32_t {};

using allocator = acpp::handle_allocator<thing_id, 20u, 12u>;

struct failure {
    bool ok{true};
    unsigned seed{};
    const char *what{};
    std::size_t step{};
};

// Split so the version cannot lap during a run: with 12 version bits a slot
// needs 4,095 releases to wrap, and the sequences below are far shorter. The
// wraparound case is version_wraparound.cpp's job, and mixing the two would
// make a failure here ambiguous.
[[nodiscard]] failure run(const unsigned seed, const std::size_t steps) {
    std::mt19937 random{seed};
    allocator handles;

    std::vector<thing_id> live;
    std::vector<thing_id> dead; // every handle ever released
    std::unordered_map<std::uint32_t, std::uint32_t> owner_of; // index -> version, shadow model

    for(std::size_t step = 0u; step < steps; ++step) {
        // Weighted so the pool grows early and churns later, which is what
        // produces long free-list chains and repeated slot reuse.
        const bool allocate = live.empty() || (random() % 100u) < (step < steps / 4u ? 80u : 45u);

        if(allocate) {
            const auto handle = handles.allocate();

            if(allocator::is_null(handle)) {
                return {false, seed, "allocate() returned null before exhausting the index space", step};
            }
            if(!handles.is_valid(handle)) {
                return {false, seed, "a freshly allocated handle did not validate", step};
            }

            const auto index = acpp::to_index(handle);
            const auto version = acpp::to_version(handle);

            // The shadow model: the allocator must never hand out an index that
            // is already live, and never with a version it has used before for
            // a live handle.
            if(const auto it = owner_of.find(index); it != owner_of.end() && it->second == version) {
                return {false, seed, "the same index/version pair was issued twice", step};
            }

            owner_of[index] = version;
            live.push_back(handle);
        } else {
            const auto pos = random() % live.size();
            const auto handle = live[pos];

            if(!handles.release(handle)) {
                return {false, seed, "release() refused a live handle", step};
            }

            live[pos] = live.back();
            live.pop_back();
            dead.push_back(handle);
            owner_of.erase(acpp::to_index(handle));
        }

        // THE PROPERTY. Checked every step, against every handle ever released,
        // not just the most recent one -- the interesting failures are the ones
        // where an old handle becomes valid again several reuses later.
        for(const auto stale: dead) {
            if(handles.is_valid(stale)) {
                return {false, seed, "a released handle validated again", step};
            }
        }

        // And the converse, which is the property that makes the first one
        // non-trivial: a validator that always says no would pass on its own.
        for(const auto handle: live) {
            if(!handles.is_valid(handle)) {
                return {false, seed, "a live handle stopped validating", step};
            }
        }
    }

    return {true, seed, nullptr, steps};
}

} // namespace

int main() {
    acpp::testing::suite suite{"module 05 / handle_property_test"};

    // 200 seeds x 400 steps. The inner loops are quadratic in the released set,
    // so this is the point where the run time is still under a second and the
    // coverage is real.
    failure first_failure{};
    std::size_t seeds_run = 0u;

    for(unsigned seed = 1u; seed <= 200u; ++seed) {
        const auto result = run(seed, 400u);
        ++seeds_run;

        if(!result.ok) {
            first_failure = result;
            break;
        }
    }

    if(!first_failure.ok) {
        suite.note("seed %u failed at step %zu: %s", first_failure.seed, first_failure.step, first_failure.what);
    }

    suite.check(first_failure.ok, "no stale handle validated across 200 randomized sequences");
    suite.note("%zu seeds x 400 operations", seeds_run);

    // A directed case the random walk is unlikely to produce: the same slot
    // released and reallocated many times, with every generation of handle kept.
    {
        allocator handles;
        std::vector<thing_id> generations;

        for(int i = 0; i < 500; ++i) {
            const auto handle = handles.allocate();
            generations.push_back(handle);
            (void)handles.release(handle);
        }

        bool any_stale_valid = false;
        for(const auto handle: generations) {
            any_stale_valid = any_stale_valid || handles.is_valid(handle);
        }

        suite.check(!any_stale_valid, "500 generations of the same slot: none validates");
        suite.check(handles.size() == 1u, "and all 500 reused one slot");
    }

    return suite.report();
}
