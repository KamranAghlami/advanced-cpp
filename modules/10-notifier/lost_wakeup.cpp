// Module 10, exercise 2 -- build a BROKEN pool and make the lost wakeup
// reproduce reliably.
//
// Getting a hang to reproduce deterministically is the skill this exercise is
// really about. A race that shows up once a week in production is not a bug you
// can fix; a race that shows up every run in three seconds is.
//
// The broken protocol is check-then-sleep with no two-phase commit:
//
//   worker: queue empty?  yes
//   pusher: push; notify  -- nobody registered, signal goes nowhere
//   worker: sleep         -- forever, with work pending
//
// The window between the check and the sleep is the whole bug. This file widens
// it on purpose so the race is not a matter of luck, then shows the same test
// passing against the 2PC notifier.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

#include <acpp/notifier.hpp>
#include <acpp/testing.hpp>

namespace {

using namespace std::chrono_literals;

/**
 * The naive notifier: no pre-wait registration at all.
 *
 * notify_one() wakes whoever happens to be parked *at that instant*. A worker
 * that has decided to sleep but has not yet reached wait() is invisible to it.
 */
class naive_notifier {
public:
    void notify_one() {
        // Deliberately not taking the mutex: this is the "cheap notify" that
        // the naive design is reaching for, and the reason the signal can slip
        // between a worker's check and its wait().
        cv.notify_one();
    }

    void wait_until_signaled() {
        std::unique_lock guard{mutex};
        cv.wait_for(guard, 50ms); // bounded so a hung test still terminates
    }

private:
    std::mutex mutex;
    std::condition_variable cv;
};

/** Where the pusher is, so the worker can widen the window deterministically. */
enum class phase : int { idle, pushed, notified };

} // namespace

int main() {
    acpp::testing::suite suite{"module 10 / lost_wakeup"};

    // --- the bug, reproduced ------------------------------------------------
    //
    // The worker checks the predicate, then blocks on a handshake until the
    // pusher has both pushed AND notified, and only then sleeps. That is
    // exactly the interleaving the race needs, made deterministic instead of
    // waited for.
    {
        naive_notifier notifier;
        std::atomic<int> queue{0};
        std::atomic<phase> where{phase::idle};
        std::atomic<bool> worker_checked{false};
        std::atomic<bool> worker_woke{false};

        std::thread worker{[&] {
            // 1. check the predicate: empty.
            const bool had_work = queue.load(std::memory_order_acquire) > 0;
            worker_checked.store(true, std::memory_order_release);

            if(!had_work) {
                // 2. the window. Let the pusher run all the way through.
                while(where.load(std::memory_order_acquire) != phase::notified) {
                    std::this_thread::yield();
                }

                // 3. sleep -- after the signal has already been and gone.
                notifier.wait_until_signaled();
            }

            worker_woke.store(true, std::memory_order_release);
        }};

        while(!worker_checked.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }

        queue.fetch_add(1, std::memory_order_release);
        where.store(phase::pushed, std::memory_order_release);
        notifier.notify_one();
        where.store(phase::notified, std::memory_order_release);

        const auto started = std::chrono::steady_clock::now();
        worker.join();
        const auto slept = std::chrono::steady_clock::now() - started;

        const auto slept_ms = std::chrono::duration<double, std::milli>{slept}.count();
        suite.note("naive: worker slept %.1f ms with work already queued", slept_ms);

        // It only returned because wait_for has a timeout. Without that bound
        // this join never completes -- which is what the bug looks like in
        // production: a pool that stops making progress and cannot say why.
        suite.check(slept_ms > 20.0,
                    "BUG REPRODUCED: the worker slept through a notify that preceded its wait");
        suite.check(queue.load() == 1, "and the work was sitting there the whole time");
    }

    // --- the same interleaving, against the two-phase protocol --------------
    {
        acpp::blocking_notifier notifier{1u};
        std::atomic<int> queue{0};
        std::atomic<phase> where{phase::idle};
        std::atomic<bool> worker_prepared{false};

        std::thread worker{[&] {
            // 1. announce the intent to sleep FIRST.
            notifier.prepare_wait(0u);
            worker_prepared.store(true, std::memory_order_release);

            // 2. now re-check. This is the whole protocol: the announcement is
            //    published before the check, so a notify arriving after the
            //    check still finds a registered pre-waiter.
            while(where.load(std::memory_order_acquire) != phase::notified) {
                std::this_thread::yield();
            }

            if(queue.load(std::memory_order_acquire) > 0) {
                notifier.cancel_wait(0u);
            } else {
                notifier.commit_wait(0u);
            }
        }};

        while(!worker_prepared.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }

        queue.fetch_add(1, std::memory_order_release);
        where.store(phase::pushed, std::memory_order_release);
        notifier.notify_one();
        where.store(phase::notified, std::memory_order_release);

        const auto started = std::chrono::steady_clock::now();
        worker.join();
        const auto elapsed = std::chrono::duration<double, std::milli>{
            std::chrono::steady_clock::now() - started}.count();

        suite.note("2PC: worker returned in %.1f ms", elapsed);
        suite.check(elapsed < 20.0, "the two-phase protocol does not lose the wakeup");
    }

    // --- and the harder case: the worker commits anyway ---------------------
    //
    // Above, the worker found work and cancelled. The interesting path is the
    // one where it re-checks, finds nothing, and parks -- while a notify is in
    // flight. The signal must be held for it, not dropped.
    {
        for(auto &notifier_pair: std::vector<int>{0}) {
            (void)notifier_pair;

            acpp::blocking_notifier notifier{1u};
            std::atomic<bool> prepared{false};
            std::atomic<bool> finished{false};

            std::thread worker{[&] {
                notifier.prepare_wait(0u);
                prepared.store(true, std::memory_order_release);
                // Predicate still false: park.
                notifier.commit_wait(0u);
                finished.store(true, std::memory_order_release);
            }};

            while(!prepared.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            // The notify races the commit. Either it finds a pre-waiter and
            // holds the signal, or it finds a parked waiter and wakes it.
            // Both outcomes must end the worker.
            notifier.notify_one();

            worker.join();
            suite.check(finished.load(), "a notify racing commit_wait still wakes the waiter");
        }
    }

    // --- the same, on the lock-free notifier --------------------------------
    {
        acpp::nonblocking_notifier notifier{1u};
        std::atomic<bool> prepared{false};
        std::atomic<bool> finished{false};

        std::thread worker{[&] {
            notifier.prepare_wait(0u);
            prepared.store(true, std::memory_order_release);
            notifier.commit_wait(0u);
            finished.store(true, std::memory_order_release);
        }};

        while(!prepared.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }

        notifier.notify_one();
        worker.join();
        suite.check(finished.load(), "the lock-free notifier closes the same window");
    }

    return suite.report();
}
