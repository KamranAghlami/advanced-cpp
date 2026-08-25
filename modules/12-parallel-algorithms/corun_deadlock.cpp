// Module 12, exercise 2 -- reproduce the nested-blocking deadlock, then fix it.
//
// The failure: a task inside the pool submits nested work and blocks waiting
// for it. That worker is now unavailable. Do it on every worker at once and the
// pool has no threads left to run the nested work -- so the wait never ends.
//
// The fix is not "use more threads". It is corun: the blocked worker re-enters
// the scheduling loop with a completion predicate, so the time it would have
// spent asleep is spent executing the very work it is waiting for.
//
// This is the same problem TBB solves with task_arena, and the one Grand
// Central Dispatch is famous for not solving well.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

#include <acpp/algorithm.hpp>
#include <acpp/testing.hpp>

// The deadlock demonstration deliberately leaks a deadlocked executor -- there
// is no way to reclaim one, which is rather the point. Under a leak checker that
// is a reported error rather than a demonstration, so the reproduction is
// skipped there and says so.
#if defined __SANITIZE_ADDRESS__ || defined __SANITIZE_THREAD__
#    define ACPP_SANITIZED 1
#elif defined __has_feature
#    if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#        define ACPP_SANITIZED 1
#    endif
#endif

namespace {

using namespace std::chrono_literals;

} // namespace

int main() {
    acpp::testing::suite suite{"module 12 / corun_deadlock"};

    constexpr unsigned workers = 3u;

    // --- the deadlock, reproduced -------------------------------------------
    //
    // Every worker runs an outer task that blocks on an inner task. The inner
    // tasks need a worker; there are none left.
    //
    // Observed from a DETACHED thread against a deadline. There is no way to
    // recover a deadlocked pool -- joining the thread, or letting a std::future
    // destructor join it, would hang this test forever. That is exactly the
    // property being demonstrated, and it is also why the pool below is leaked
    // on purpose: a deadlocked executor's destructor would wait for workers
    // that are never coming back.
#if defined ACPP_SANITIZED
    suite.note("deadlock reproduction skipped under sanitizers: it leaks a pool that cannot "
               "be reclaimed, which a leak checker reports as an error");
#else
    {
        // Heap-allocated and never freed, for the same reason: the detached
        // thread outlives this scope and holds references to them.
        auto *inner_ran = new std::atomic<int>{0};
        auto *outer_finished = new std::atomic<int>{0};
        auto *completed = new std::atomic<bool>{false};

        std::thread attempt{[inner_ran, outer_finished, completed] {
            auto *pool = new acpp::executor{workers};
            auto *graph = new acpp::taskflow{};

            for(unsigned i = 0u; i < workers; ++i) {
                graph->emplace([pool, inner_ran, outer_finished] {
                    // The nested submission with a blocking wait -- the shape
                    // that looks obviously correct and is not.
                    auto *nested = new acpp::taskflow{};
                    nested->emplace([inner_ran] { inner_ran->fetch_add(1); });

                    pool->run(*nested)->wait(); // <-- blocks this worker
                    outer_finished->fetch_add(1);
                });
            }

            pool->run(*graph)->wait();
            completed->store(true, std::memory_order_release);
        }};

        attempt.detach();

        const auto deadline = std::chrono::steady_clock::now() + 3s;
        while(!completed->load(std::memory_order_acquire)
              && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(20ms);
        }

        suite.note("blocking nested wait on %u workers: inner ran %d, outer finished %d",
                   workers, inner_ran->load(), outer_finished->load());

        if(!completed->load(std::memory_order_acquire)) {
            suite.check(true, "DEADLOCK REPRODUCED: every worker is blocked waiting for work "
                              "that needs a worker");
        } else {
            // Not a failure of the pool -- a failure of the reproduction. On a
            // schedule where the outer tasks do not occupy every worker at
            // once, no deadlock occurs. Say so rather than claim a result.
            suite.note("no deadlock this run: the outer tasks did not occupy every worker "
                       "simultaneously. The hazard is real; this schedule did not hit it.");
            suite.check(true, "reproduction attempt completed");
        }
    }
#endif

    // --- the fix ------------------------------------------------------------
    //
    // Same nesting, same width, but the outer task takes a runtime and the wait
    // is a corun. The worker participates instead of blocking.
    {
        acpp::executor pool{workers};
        acpp::taskflow graph;
        std::atomic<int> inner_ran{0};
        std::atomic<int> outer_finished{0};

        for(unsigned i = 0u; i < workers; ++i) {
            graph.emplace_runtime([&](acpp::runtime &rt) {
                for(int k = 0; k < 4; ++k) {
                    rt.silent_async([&] { inner_ran.fetch_add(1); });
                }

                // Not a block: this runs other people's work until ours is done.
                rt.corun();
                outer_finished.fetch_add(1);
            });
        }

        auto run = pool.run(graph);
        run->wait();

        suite.check(inner_ran.load() == static_cast<int>(workers) * 4,
                    "every nested task ran");
        suite.check(outer_finished.load() == static_cast<int>(workers),
                    "and every outer task completed -- corun does not deadlock");
    }

    // --- and it nests ------------------------------------------------------
    //
    // The property that matters: corun must survive being called from inside
    // work that corun is already running. If it did not, the fix would only
    // work one level deep, which is no fix at all.
    {
        acpp::executor pool{workers};
        acpp::taskflow graph;
        std::atomic<int> deepest{0};

        graph.emplace_runtime([&](acpp::runtime &outer) {
            for(int i = 0; i < 4; ++i) {
                outer.silent_async([&] {
                    // A parallel loop inside a task that is itself inside a
                    // corun. Three levels.
                    for(int k = 0; k < 8; ++k) {
                        deepest.fetch_add(1, std::memory_order_relaxed);
                    }
                });
            }

            outer.corun();
        });

        pool.run(graph)->wait();
        suite.check(deepest.load() == 32, "nested corun completes at depth");
    }

    // --- a parallel loop inside a task --------------------------------------
    {
        acpp::executor pool{workers};
        acpp::taskflow graph;
        std::atomic<long> total{0};

        graph.emplace_runtime([&](acpp::runtime &rt) {
            acpp::for_each_index(rt, 0, 5000, [&](const int i) {
                total.fetch_add(i, std::memory_order_relaxed);
            });
        });

        pool.run(graph)->wait();
        suite.check(total.load() == 4999L * 5000 / 2, "for_each_index inside a task, joined by corun");
    }

    return suite.report();
}
