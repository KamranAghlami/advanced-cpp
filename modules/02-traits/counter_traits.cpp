// Module 2, exercise 3 -- the macro this repo actually had, converted to a trait.
//
// ACPP_MAYBE_ATOMIC(Type) was introduced in Module 1 to decide whether the
// type-id counter is atomic. It had the three problems every policy macro has:
// one answer for the whole build, invisible at the point of use, and no way for
// a caller to disagree without recompiling the world.
//
// src/acpp/counter.hpp replaces it with counter_traits<Tag>. The macro survives
// only as a way to move the inferred default.

#include <atomic>
#include <thread>
#include <type_traits>
#include <vector>

#include <acpp/counter.hpp>
#include <acpp/testing.hpp>
#include <acpp/type_info.hpp>

namespace {

// Level 1: inferred. A hosted, threaded build gets an atomic counter without
// anyone writing anything.
struct default_tag {};

static_assert(acpp::sequential_counter<default_tag>::is_atomic == (
#if defined __STDCPP_THREADS__
    true
#else
    false
#endif
    ), "the default must follow __STDCPP_THREADS__");

// Level 2: a counter that is provably single-threaded opts out from inside its
// own tag. This is the rung the macro could never offer -- it is per counter,
// not per build.
struct single_threaded_tag {
    static constexpr bool atomic_counter = false;
};

static_assert(!acpp::sequential_counter<single_threaded_tag>::is_atomic);

// Level 3: full override.
struct third_party_tag {};

} // namespace

namespace acpp {

template<>
struct counter_traits<third_party_tag> {
    static constexpr bool atomic = false;
};

} // namespace acpp

namespace {

static_assert(!acpp::sequential_counter<third_party_tag>::is_atomic);

// Distinct tags are distinct counters. Worth pinning: the whole reason to key a
// counter on a tag type is to stop unrelated subsystems sharing a sequence.
struct tag_a {};
struct tag_b {};

} // namespace

int main() {
    acpp::testing::suite suite{"module 02 / counter_traits"};

    suite.check(acpp::sequential_counter<tag_a>::next() == 0u, "a fresh counter starts at zero");
    suite.check(acpp::sequential_counter<tag_a>::next() == 1u, "and increments");
    suite.check(acpp::sequential_counter<tag_b>::next() == 0u, "a different tag is a different counter");

    suite.check(!acpp::sequential_counter<single_threaded_tag>::is_atomic, "level 2: member opts out");
    suite.check(!acpp::sequential_counter<third_party_tag>::is_atomic, "level 3: specialization opts out");

    // What the atomic default is actually for. Many threads asking for the ids
    // of many *different* types all reach the shared increment; the per-type
    // static-local guard serialises each initialiser but not the counter.
    // Every id handed out must still be distinct.
    if constexpr(acpp::sequential_counter<default_tag>::is_atomic) {
        constexpr int per_thread = 2000;
        const unsigned workers = 4u;
        std::vector<std::thread> threads;
        std::atomic<unsigned> sum{0u};
        std::atomic<unsigned> xored{0u};

        for(unsigned i = 0u; i < workers; ++i) {
            threads.emplace_back([&] {
                unsigned local_sum = 0u;
                unsigned local_xor = 0u;
                for(int n = 0; n < per_thread; ++n) {
                    const auto id = acpp::sequential_counter<default_tag>::next();
                    local_sum += id;
                    local_xor ^= id;
                }
                sum += local_sum;
                xored ^= local_xor;
            });
        }

        for(auto &thread: threads) {
            thread.join();
        }

        // The ids handed out must be exactly 0 .. (workers*per_thread - 1), in
        // some order. Sum and xor together pin the multiset tightly enough that
        // a duplicate or a gap shows up.
        const unsigned count = workers * per_thread;
        unsigned expected_sum = 0u;
        unsigned expected_xor = 0u;
        for(unsigned n = 0u; n < count; ++n) {
            expected_sum += n;
            expected_xor ^= n;
        }

        suite.check(sum.load() == expected_sum && xored.load() == expected_xor,
                    "8000 concurrent draws produced every id exactly once");
    } else {
        suite.note("build is not threaded; skipping the contention check");
    }

    // And the reason any of this exists: type_index is built on it.
    suite.check(acpp::type_index<int>::value() != acpp::type_index<double>::value(),
                "type_index still hands out distinct ids through the trait-driven counter");

    return suite.report();
}
