// Module 0 -- Taskflow smoke test.
//
// Proves the pinned Taskflow checkout compiles, links and runs here, and drives
// exactly the machinery Phase C dissects: the work-stealing executor (Modules
// 9-10), the join-counter DAG (Module 11), and a partitioned parallel loop
// (Module 12).

#include <atomic>
#include <numeric>
#include <thread>
#include <vector>

#include <acpp/testing.hpp>

#include <taskflow/algorithm/for_each.hpp>
#include <taskflow/taskflow.hpp>

int main() {
    acpp::testing::suite suite{"module 00 / taskflow_smoke"};
    suite.note("Taskflow %d.%d.%d (pinned c4da2a4)", TF_MAJOR_VERSION, TF_MINOR_VERSION,
               TF_PATCH_VERSION);

    tf::Executor executor;
    suite.note("executor.num_workers() = %zu, hardware_concurrency() = %u", executor.num_workers(),
               std::thread::hardware_concurrency());

    // Module 11: a diamond. B and C both decrement D's join counter; whoever
    // drives it to zero schedules D. Recording the order proves the dependency
    // mechanism actually held rather than everything happening to run in order.
    std::atomic<int> stamp{0};
    int order[4] = {-1, -1, -1, -1};

    tf::Taskflow flow;
    auto [a, b, c, d] = flow.emplace([&] { order[0] = stamp++; }, [&] { order[1] = stamp++; },
                                     [&] { order[2] = stamp++; }, [&] { order[3] = stamp++; });
    a.precede(b, c);
    d.succeed(b, c);

    executor.run(flow).wait();

    suite.check(order[0] == 0, "root task ran first");
    suite.check(order[3] == 3, "join task ran last");
    suite.check(order[1] != order[2], "the two middle tasks got distinct stamps");

    // Module 12: parallel_for over the default partitioner. Correctness only --
    // any timing claim on this machine would be worthless (check nproc).
    std::vector<int> data(10'000);
    std::iota(data.begin(), data.end(), 0);

    tf::Taskflow loop;
    loop.for_each(data.begin(), data.end(), [](int &value) { value *= 2; });
    executor.run(loop).wait();

    const long long sum = std::accumulate(data.begin(), data.end(), 0LL);
    suite.check(sum == 10'000LL * 9'999LL, "for_each doubled every element exactly once");

    // Module 12: corun -- a worker that needs a nested graph finished executes it
    // itself rather than parking. Writing `executor.run(inner).wait()` here would
    // block the worker instead, which is the deadlock §12.2 describes (Taskflow's
    // own doc comment on Executor::corun calls it out in the same words).
    // Note this is Executor::corun; Runtime::corun() is a different thing in 4.1 --
    // it waits on tasks spawned by that runtime and takes no argument.
    std::atomic<int> nested{0};
    tf::Taskflow inner;
    inner.for_each_index(0, 100, 1, [&](int) { nested.fetch_add(1, std::memory_order_relaxed); });

    tf::Taskflow outer;
    outer.emplace([&] { executor.corun(inner); });
    executor.run(outer).wait();

    suite.check(nested.load() == 100, "corun executed the nested graph to completion");

    return suite.report();
}
