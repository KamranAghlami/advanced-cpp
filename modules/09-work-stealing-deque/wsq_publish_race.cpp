// Module 9, exercise 3 -- the weakening ARM can catch, with the window held open.
//
// Companion to wsq_stress.cpp, and it exists because that test cannot see this
// bug. wsq_stress pushes three items for every pop, so the queue grows to 65,536
// slots and thieves work at `top` while the owner works at `bottom` -- thousands
// of slots apart. The publish race needs them on the SAME slot: a thief must read
// the very entry the owner is in the middle of publishing.
//
// So this keeps the queue 0-2 deep. Every steal then contends with the push that
// is publishing it, which is the only interleaving that can observe the missing
// release. Measured on an M1: with -DACPP_WSQ_WEAKEN_RELEASE it corrupts on
// roughly a quarter of runs; without it, never. Numbers in NOTES.md.
//
// What goes wrong, concretely. `push` writes the slot with a relaxed store and
// then publishes it by storing the incremented `bottom`. With the publish store
// demoted to relaxed there is no release/acquire edge to the thief's
// `bottom.load(acquire)` in `steal`, so the two stores may be observed out of
// order and the thief reads a slot the owner has not written yet. The buffer is
// tiny here, so what it reads is the PREVIOUS generation's pointer: that item
// gets consumed a second time and the newly-pushed one is never consumed at all.
// Hence the two counters below always move together.

#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>

#include <acpp/testing.hpp>
#include <acpp/wsq.hpp>

namespace {

constexpr int total_items = 200'000;

// More thieves than half the cores, so somebody is always racing the owner
// rather than waiting for a turn.
constexpr unsigned thieves = 6u;

} // namespace

int main() {
    acpp::testing::suite suite{"module 09 / wsq_publish_race"};

#if defined ACPP_WSQ_WEAKEN_RELEASE
    suite.note("BUILT WITH THE PUBLISH STORE DELIBERATELY WEAKENED (relaxed instead of release)");
    suite.note("this build is EXPECTED TO FAIL on a weakly-ordered machine -- that is the result");
#endif

    const auto cores = std::thread::hardware_concurrency();
    suite.note("hardware_concurrency() = %u", cores);

    if(cores < 2u) {
        suite.note("one core: the owner and thieves cannot overlap, so the window "
                   "this program exists to open stays shut. A pass here means nothing.");
    }

    std::vector<int> items(total_items);
    for(int i = 0; i < total_items; ++i) {
        items[static_cast<std::size_t>(i)] = i;
    }

    // Per-item consumption count. A stale slot read is visible as one item taken
    // twice and another taken zero times, so both counters are reported: seeing
    // them equal is what identifies the failure as a lost publish rather than as
    // a plain lost or duplicated item.
    std::vector<std::atomic<int>> consumed(total_items);
    for(auto &slot: consumed) {
        slot.store(0, std::memory_order_relaxed);
    }

    const auto take = [&consumed](int *item) noexcept {
        consumed[static_cast<std::size_t>(*item)].fetch_add(1, std::memory_order_relaxed);
    };

    // Capacity 8: the ring wraps every few pushes, so a stale read lands on a
    // recent generation and is guaranteed to be a *different* item.
    acpp::bounded_wsq<int *, 8> queue{};
    std::atomic<bool> producing{true};
    std::atomic<long> stolen{0};

    std::vector<std::thread> workers;
    workers.reserve(thieves);

    for(unsigned id = 0u; id < thieves; ++id) {
        workers.emplace_back([&] {
            while(producing.load(std::memory_order_relaxed) || !queue.empty()) {
                if(auto *item = queue.steal(); item != nullptr) {
                    take(item);
                    stolen.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    long popped = 0;

    for(int i = 0; i < total_items; ++i) {
        // Blocking on a full queue rather than growing is what holds the depth
        // down; the owner drains its own bottom when the thieves fall behind.
        while(!queue.try_push(&items[static_cast<std::size_t>(i)])) {
            if(auto *item = queue.pop(); item != nullptr) {
                take(item);
                ++popped;
            }
        }

        if((i & 1) == 0) {
            if(auto *item = queue.pop(); item != nullptr) {
                take(item);
                ++popped;
            }
        }
    }

    producing.store(false, std::memory_order_relaxed);

    while(auto *item = queue.pop()) {
        take(item);
        ++popped;
    }

    for(auto &worker: workers) {
        worker.join();
    }

    int duplicated = 0;
    int lost = 0;

    for(int i = 0; i < total_items; ++i) {
        const auto count = consumed[static_cast<std::size_t>(i)].load(std::memory_order_relaxed);

        if(count > 1) {
            ++duplicated;
        } else if(count == 0) {
            ++lost;
        }
    }

    suite.note("owner popped %ld, thieves stole %ld", popped, stolen.load());
    suite.note("duplicated %d, lost %d", duplicated, lost);

    suite.check(duplicated == 0, "no item was consumed twice");
    suite.check(lost == 0, "no item was dropped");

    return suite.report();
}
