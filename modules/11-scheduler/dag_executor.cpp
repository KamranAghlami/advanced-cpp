// Module 11, exercise 3 -- the integration point for all of Phase C.
//
//   Module 9's work-stealing deque  -> per-worker queues
//   Module 10's notifier            -> sleeping without lost wakeups
//   Module 11's graph               -> variant nodes, atomic join counters
//
// Everything below is about whether the *dependencies* are honoured, which is
// the only thing a DAG executor is for.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

#include <acpp/executor.hpp>
#include <acpp/testing.hpp>

namespace {

using namespace std::chrono_literals;

/** Records the order tasks ran in, so "before" can be asserted rather than hoped. */
class trace {
public:
    void record(const int id) {
        const std::lock_guard guard{mutex};
        order.push_back(id);
    }

    [[nodiscard]] bool before(const int first, const int second) const {
        const std::lock_guard guard{mutex};
        std::size_t at_first = order.size();
        std::size_t at_second = order.size();

        for(std::size_t pos = 0u; pos < order.size(); ++pos) {
            if(order[pos] == first && at_first == order.size()) {
                at_first = pos;
            }
            if(order[pos] == second && at_second == order.size()) {
                at_second = pos;
            }
        }

        return at_first < at_second;
    }

    [[nodiscard]] std::size_t size() const {
        const std::lock_guard guard{mutex};
        return order.size();
    }

private:
    mutable std::mutex mutex;
    std::vector<int> order;
};

} // namespace

int main() {
    acpp::testing::suite suite{"module 11 / dag_executor"};

    acpp::executor pool{4u};
    suite.note("workers = %u, hardware_concurrency() = %u", pool.num_workers(),
               std::thread::hardware_concurrency());

    // --- a diamond ----------------------------------------------------------
    //
    //          A
    //        /   |
    //       B    C
    //        |   /
    //          D
    //
    // D must run after both B and C, and exactly once -- that is the join
    // counter's entire job.
    {
        acpp::taskflow graph;
        trace log;
        std::atomic<int> d_runs{0};

        auto a = graph.emplace([&] { log.record(0); });
        auto b = graph.emplace([&] { log.record(1); });
        auto c = graph.emplace([&] { log.record(2); });
        auto d = graph.emplace([&] { log.record(3); d_runs.fetch_add(1); });

        a.precede(b);
        a.precede(c);
        b.precede(d);
        c.precede(d);

        pool.run(graph)->wait();

        suite.check(log.size() == 4u, "all four tasks ran");
        suite.check(d_runs.load() == 1, "the join node ran exactly once, not once per predecessor");
        suite.check(log.before(0, 1) && log.before(0, 2), "A before B and C");
        suite.check(log.before(1, 3) && log.before(2, 3), "B and C before D");
    }

    // --- a long chain -------------------------------------------------------
    //
    // This is where the continuation cache should do its work: every task has
    // exactly one ready successor, so the whole chain should run on one worker
    // with no queue traffic at all.
    {
        acpp::taskflow graph;
        std::atomic<int> counter{0};
        std::vector<acpp::task> chain;

        constexpr int length = 200;
        for(int i = 0; i < length; ++i) {
            chain.push_back(graph.emplace([&counter, i] {
                // Each link checks it ran in order.
                if(counter.load(std::memory_order_acquire) == i) {
                    counter.store(i + 1, std::memory_order_release);
                }
            }));
        }

        for(int i = 0; i + 1 < length; ++i) {
            chain[static_cast<std::size_t>(i)].precede(chain[static_cast<std::size_t>(i + 1)]);
        }

        pool.reset_stats();
        pool.run(graph)->wait();
        const auto stats = pool.stats();

        suite.check(counter.load() == length, "a 200-link chain ran strictly in order");
        suite.note("chain of %d: %zu continuations, %zu successful steals",
                   length, stats.continuations, stats.steals_succeeded);
        suite.check(stats.continuations >= static_cast<std::size_t>(length) - 2u,
                    "and almost every link was a direct continuation, not a queue round-trip");
    }

    // --- fan-out ------------------------------------------------------------
    //
    // The opposite case: one task with many successors is where queue traffic
    // is supposed to happen, because that is where parallelism comes from.
    {
        acpp::taskflow graph;
        std::atomic<int> leaves{0};

        auto root = graph.emplace([] {});
        auto join = graph.emplace([] {});

        constexpr int width = 64;
        for(int i = 0; i < width; ++i) {
            auto leaf = graph.emplace([&leaves] { leaves.fetch_add(1, std::memory_order_relaxed); });
            root.precede(leaf);
            leaf.precede(join);
        }

        pool.run(graph)->wait();
        suite.check(leaves.load() == width, "all 64 parallel tasks ran");
    }

    // --- conditions ---------------------------------------------------------
    //
    // A condition task returns the index of the ONE successor to take. The
    // others are simply not taken; their join counters are left alone.
    {
        acpp::taskflow graph;
        std::atomic<int> taken{-1};
        std::atomic<int> other_ran{0};

        auto pick = graph.emplace_condition([] { return 1; });
        auto first = graph.emplace([&] { taken.store(0); other_ran.fetch_add(1); });
        auto second = graph.emplace([&] { taken.store(1); });

        pick.precede(first);
        pick.precede(second);

        pool.run(graph)->wait();

        suite.check(taken.load() == 1, "the condition took branch 1");
        suite.check(other_ran.load() == 0, "and branch 0 did not run");
    }

    // --- re-running the same graph ------------------------------------------
    //
    // The reset problem: a graph that runs twice must have its join counters
    // restored, and by somebody. Here it is run(), before anything is
    // scheduled -- which is the only point at which no worker can be looking.
    {
        acpp::taskflow graph;
        std::atomic<int> total{0};

        auto a = graph.emplace([&] { total.fetch_add(1); });
        auto b = graph.emplace([&] { total.fetch_add(10); });
        auto c = graph.emplace([&] { total.fetch_add(100); });
        a.precede(c);
        b.precede(c);

        for(int pass = 0; pass < 5; ++pass) {
            pool.run(graph)->wait();
        }

        suite.check(total.load() == 5 * 111, "five runs of the same graph, each complete");
    }

    // --- degenerate shapes --------------------------------------------------
    {
        acpp::taskflow empty;
        pool.run(empty)->wait();
        suite.check(true, "an empty graph completes rather than hanging");

        acpp::taskflow single;
        std::atomic<bool> ran{false};
        single.emplace([&] { ran.store(true); });
        pool.run(single)->wait();
        suite.check(ran.load(), "a one-node graph runs");

        // A cycle has no source, so nothing can ever start. Saying so beats
        // hanging, and hanging is the default if you do not check.
        acpp::taskflow cycle;
        auto x = cycle.emplace([] {});
        auto y = cycle.emplace([] {});
        x.precede(y);
        y.precede(x);

        auto run = pool.run(cycle);
        run->wait();
        suite.check(run->cancelled(), "a graph with no source is reported, not hung");
    }

    // --- concurrent submissions ---------------------------------------------
    {
        constexpr int graphs = 8;
        std::atomic<int> total{0};
        std::vector<acpp::taskflow> flows(graphs);
        std::vector<std::unique_ptr<acpp::topology>> runs;

        for(int i = 0; i < graphs; ++i) {
            auto a = flows[static_cast<std::size_t>(i)].emplace([&] { total.fetch_add(1); });
            auto b = flows[static_cast<std::size_t>(i)].emplace([&] { total.fetch_add(1); });
            a.precede(b);
        }

        for(int i = 0; i < graphs; ++i) {
            runs.push_back(pool.run(flows[static_cast<std::size_t>(i)]));
        }

        for(auto &run: runs) {
            run->wait();
        }

        suite.check(total.load() == graphs * 2, "eight graphs in flight at once all completed");
    }

    return suite.report();
}
