// Module 10, exercises 3 and 4 -- the protocol under load, both implementations.
//
// A minimal pool: one shared queue, N workers, and the two-phase wait around
// the empty case. Deliberately NOT a work-stealing pool -- Module 11 builds
// that. Everything here is about the notifier, so the queue is as boring as
// possible and any hang is the protocol's fault.
//
// Run under TSan; that is the Phase C ground rule and this is the file it is
// about:
//   setarch $(uname -m) -R ./build/verify/tsan/modules/10-notifier/notifier_protocol

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

#include <acpp/notifier.hpp>
#include <acpp/testing.hpp>

namespace {

using namespace std::chrono_literals;

/**
 * The smallest pool that can exhibit a lost wakeup.
 *
 * The whole point is the shape of `worker()`: every exit from the wait window
 * calls exactly one of cancel_wait or commit_wait. Trace all four.
 */
template<typename Notifier>
class tiny_pool {
public:
    explicit tiny_pool(const unsigned workers)
        : notifier{workers}, threads{}, done{false} {
        threads.reserve(workers);

        for(unsigned id = 0u; id < workers; ++id) {
            threads.emplace_back([this, id] { worker(id); });
        }
    }

    ~tiny_pool() {
        done.store(true, std::memory_order_release);
        notifier.notify_all();

        for(auto &thread: threads) {
            thread.join();
        }
    }

    void submit(const int item) {
        {
            const std::lock_guard guard{mutex};
            items.push(item);
        }

        // The hot path: no lock on the notifier, and cheap when nobody sleeps.
        notifier.notify_one();
    }

    [[nodiscard]] std::uint64_t consumed_sum() const { return sum.load(std::memory_order_acquire); }
    [[nodiscard]] int consumed_count() const { return count.load(std::memory_order_acquire); }
    [[nodiscard]] std::uint64_t sleeps() const { return parks.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t cancels() const { return aborts.load(std::memory_order_relaxed); }

private:
    [[nodiscard]] bool take(int &out) {
        const std::lock_guard guard{mutex};

        if(items.empty()) {
            return false;
        }

        out = items.front();
        items.pop();
        return true;
    }

    void worker(const unsigned id) {
        int item = 0;

        for(;;) {
            while(take(item)) {
                sum.fetch_add(static_cast<std::uint64_t>(item), std::memory_order_acq_rel);
                count.fetch_add(1, std::memory_order_acq_rel);
            }

            // --- the wait window opens ---------------------------------------
            notifier.prepare_wait(id);

            // Exit 1: work appeared between the drain above and the
            // announcement. Cancel, do not park.
            if(has_work()) {
                notifier.cancel_wait(id);
                aborts.fetch_add(1u, std::memory_order_relaxed);
                continue;
            }

            // Exit 2: shutting down. Cancel, then leave. Note the order --
            // leaving without resolving the protocol is UB, and it is the
            // easiest way to get this wrong.
            if(done.load(std::memory_order_acquire)) {
                notifier.cancel_wait(id);
                return;
            }

            // Exit 3: nothing to do. Park. A notify that arrived any time after
            // prepare_wait is delivered here rather than lost.
            parks.fetch_add(1u, std::memory_order_relaxed);
            notifier.commit_wait(id);

            // Exit 4: woken. Re-check shutdown, then go round again.
            if(done.load(std::memory_order_acquire) && !has_work()) {
                return;
            }
        }
    }

    [[nodiscard]] bool has_work() {
        const std::lock_guard guard{mutex};
        return !items.empty();
    }

    Notifier notifier;
    std::vector<std::thread> threads;
    std::atomic<bool> done;

    std::mutex mutex;
    std::queue<int> items;

    std::atomic<std::uint64_t> sum{0u};
    std::atomic<int> count{0};
    std::atomic<std::uint64_t> parks{0u};
    std::atomic<std::uint64_t> aborts{0u};
};

template<typename Notifier>
void exercise(acpp::testing::suite &suite, const char *name) {
    constexpr int items = 20'000;
    constexpr unsigned workers = 4u;

    std::uint64_t expected = 0u;
    for(int i = 1; i <= items; ++i) {
        expected += static_cast<std::uint64_t>(i);
    }

    tiny_pool<Notifier> pool{workers};

    // Bursty: a tight run of submissions, then a gap long enough that workers
    // genuinely park. Both halves are needed -- the tight run exercises the
    // notify fast path, the gap exercises the park/unpark path.
    for(int burst = 0; burst < 20; ++burst) {
        for(int i = 0; i < items / 20; ++i) {
            pool.submit(burst * (items / 20) + i + 1);
        }

        std::this_thread::sleep_for(200us);
    }

    // Wait for drain, with a bound so a lost wakeup is a test failure rather
    // than a hung CI job.
    const auto deadline = std::chrono::steady_clock::now() + 30s;
    while(pool.consumed_count() < items && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }

    suite.note("%s: %d items, %llu parks, %llu cancels", name, pool.consumed_count(),
               static_cast<unsigned long long>(pool.sleeps()),
               static_cast<unsigned long long>(pool.cancels()));

    suite.check(pool.consumed_count() == items, "every item was consumed (no lost wakeup)");
    suite.check(pool.consumed_sum() == expected, "and each exactly once");
    suite.check(pool.sleeps() > 0u, "workers really did park -- otherwise this proves nothing");
}

} // namespace

int main() {
    acpp::testing::suite suite{"module 10 / notifier_protocol"};

    suite.note("hardware_concurrency() = %u", std::thread::hardware_concurrency());

    exercise<acpp::blocking_notifier>(suite, "blocking_notifier");
    exercise<acpp::nonblocking_notifier>(suite, "nonblocking_notifier");

    // The shutdown path is the one that hangs if the protocol is violated: the
    // destructor sets `done` and calls notify_all, and every worker must
    // resolve its protocol state and leave. Reaching here at all is the check.
    suite.check(true, "both pools shut down cleanly (every prepare_wait was resolved)");

    return suite.report();
}
