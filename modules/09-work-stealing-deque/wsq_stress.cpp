// Module 9, exercise 3 -- one owner, N thieves, every item consumed exactly once.
//
// Build and run under TSan for this to mean anything:
//   scripts/verify.sh -R wsq_stress
//   setarch $(uname -m) -R ./build/verify/tsan/modules/09-work-stealing-deque/wsq_stress
//
// The same source is compiled twice: once as-is and once with
// -DACPP_WSQ_WEAKEN_FENCE, which turns the two seq_cst fences into acq_rel. The
// weakened build is EXPECTED to pass on x86, and that is the lesson -- see
// NOTES.md. It is registered as a test so the expectation is recorded, not so
// the build fails.

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

#include <acpp/testing.hpp>
#include <acpp/wsq.hpp>

namespace {

constexpr int total_items = 200'000;

// Each item is consumed by exactly one thread, and each records its own id.
// Summing and xor-ing the multiset of consumed ids pins it tightly enough that
// a duplicate or a drop shows up; a plain count would miss "one twice, one
// never".
struct tally {
    std::atomic<std::uint64_t> sum{0};
    std::atomic<std::uint64_t> checksum{0};
    std::atomic<int> count{0};

    void record(const int value) noexcept {
        sum.fetch_add(static_cast<std::uint64_t>(value), std::memory_order_relaxed);
        checksum.fetch_xor(static_cast<std::uint64_t>(value) * 2654435761u, std::memory_order_relaxed);
        count.fetch_add(1, std::memory_order_relaxed);
    }
};

} // namespace

int main() {
    acpp::testing::suite suite{"module 09 / wsq_stress"};

#if defined ACPP_WSQ_WEAKEN_FENCE
    suite.note("BUILT WITH THE FENCE DELIBERATELY WEAKENED (acq_rel instead of seq_cst)");
#endif

    const auto cores = std::thread::hardware_concurrency();
    suite.note("hardware_concurrency() = %u", cores);

    if(cores < 2u) {
        suite.note("one core: thieves and owner cannot actually overlap, so this run "
                   "exercises the algorithm but not the race. TSan still models "
                   "happens-before, so a missing edge is still reported.");
    }

    std::vector<int> items(total_items);
    for(int i = 0; i < total_items; ++i) {
        items[static_cast<std::size_t>(i)] = i;
    }

    std::uint64_t expected_sum = 0u;
    std::uint64_t expected_checksum = 0u;
    for(int i = 0; i < total_items; ++i) {
        expected_sum += static_cast<std::uint64_t>(i);
        expected_checksum ^= static_cast<std::uint64_t>(i) * 2654435761u;
    }

    acpp::unbounded_wsq<int *> queue{8u};
    tally consumed;
    std::atomic<bool> producing{true};

    constexpr unsigned thieves = 3u;
    std::vector<std::thread> workers;
    std::vector<int> steals(thieves, 0);

    for(unsigned id = 0u; id < thieves; ++id) {
        workers.emplace_back([&, id] {
            int taken = 0;

            while(producing.load(std::memory_order_relaxed) || !queue.empty()) {
                if(auto *item = queue.steal(); item != nullptr) {
                    consumed.record(*item);
                    ++taken;
                } else {
                    std::this_thread::yield();
                }
            }

            steals[id] = taken;
        });
    }

    // The owner interleaves pushes and pops, which is what a real worker does
    // and what keeps `bottom` moving in both directions against the thieves.
    int popped = 0;
    for(int i = 0; i < total_items; ++i) {
        queue.push(&items[static_cast<std::size_t>(i)]);

        if((i % 3) == 0) {
            if(auto *item = queue.pop(); item != nullptr) {
                consumed.record(*item);
                ++popped;
            }
        }
    }

    while(auto *item = queue.pop()) {
        consumed.record(*item);
        ++popped;
    }

    producing.store(false, std::memory_order_relaxed);

    for(auto &worker: workers) {
        worker.join();
    }

    // Anything the thieves left behind after they stopped.
    while(auto *item = queue.pop()) {
        consumed.record(*item);
        ++popped;
    }

    int total_stolen = 0;
    for(const auto taken: steals) {
        total_stolen += taken;
    }

    suite.note("owner popped %d, thieves stole %d (%d/%d/%d)",
               popped, total_stolen, steals[0], steals[1], steals[2]);

    suite.check(consumed.count.load() == total_items, "every item was consumed");
    suite.check(consumed.sum.load() == expected_sum, "and the sum matches");
    suite.check(consumed.checksum.load() == expected_checksum,
                "and the checksum matches -- no item consumed twice, none dropped");
    suite.check(popped + total_stolen == total_items, "the two sides account for everything");
    suite.check(queue.empty(), "the queue is drained");

    suite.note("queue grew to %lld slots in %zu resizes",
               static_cast<long long>(queue.capacity()), queue.resizes());

    return suite.report();
}
