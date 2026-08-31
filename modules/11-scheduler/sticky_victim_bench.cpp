// Module 10, exercise 6 (it needs Module 11's executor) -- sticky victim.
//
// After a successful steal, remember who you stole from and try them first next
// time. Producer/consumer relationships in a task graph are stable, so the
// worker that had work a moment ago is the one most likely to have more. Cheap
// heuristic; the claim is that it converts random stealing into near-directed
// stealing and cuts the number of *attempts*.
//
// A measurement, not a test. Steal counts on a single core are dominated by
// scheduling luck, so the run reports the spread as well as the number.

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

#include <acpp/executor.hpp>

namespace {

/**
 * A pipeline-shaped graph: stages of parallel tasks, each stage joined before
 * the next fans out. That shape is what makes a victim "sticky" -- the worker
 * that expanded a stage holds the whole stage in its own deque.
 */
void build_pipeline(acpp::taskflow &graph, std::atomic<int> &counter, const int stages, const int width) {
    std::vector<acpp::task> previous;

    for(int stage = 0; stage < stages; ++stage) {
        std::vector<acpp::task> current;

        for(int i = 0; i < width; ++i) {
            current.push_back(graph.emplace([&counter] {
                // A little work, so a task is not pure scheduling overhead.
                // Long enough that the OS preempts mid-task. On a single core
                // that is the only way another worker gets scheduled at all,
                // and therefore the only way a steal can happen.
                volatile int sink = 0;
                for(int spin = 0; spin < 60'000; ++spin) {
                    sink = sink + spin;
                }
                counter.fetch_add(1, std::memory_order_relaxed);
            }));
        }

        if(!previous.empty()) {
            auto join = graph.emplace([] {});
            for(auto &task: previous) {
                task.precede(join);
            }
            for(auto &task: current) {
                join.precede(task);
            }
        }

        previous = current;
    }
}

struct sample {
    std::size_t attempts;
    std::size_t succeeded;
    std::size_t sticky_hits;
    std::size_t continuations;
    double ms;
};

[[nodiscard]] sample measure(acpp::executor &pool, const bool sticky, const int stages, const int width) {
    acpp::taskflow graph;
    std::atomic<int> counter{0};
    build_pipeline(graph, counter, stages, width);

    pool.set_sticky(sticky);
    pool.reset_stats();

    const auto started = std::chrono::steady_clock::now();
    pool.run(graph)->wait();
    const auto finished = std::chrono::steady_clock::now();

    const auto stats = pool.stats();

    if(counter.load() != stages * width) {
        std::printf("  !! %d of %d tasks ran -- the measurement is not measuring what it claims\n",
                    counter.load(), stages * width);
    }

    return {stats.steal_attempts, stats.steals_succeeded, stats.sticky_hits, stats.continuations,
            std::chrono::duration<double, std::milli>{finished - started}.count()};
}

void report(const char *label, const sample &value) {
    std::printf("%-14s %12zu %12zu %12zu %12zu %10.2f\n", label, value.attempts, value.succeeded,
                value.sticky_hits, value.continuations, value.ms);
}

} // namespace

int main() {
    constexpr int stages = 20;
    constexpr int width = 32;

    acpp::executor pool{4u};

    std::printf("pipeline: %d stages x %d tasks, %u workers, hardware_concurrency() = %u\n",
                stages, width, pool.num_workers(), std::thread::hardware_concurrency());

    if(std::thread::hardware_concurrency() < 2u) {
        std::printf("ONE CORE: workers time-slice rather than run in parallel, so steal\n"
                    "counts reflect the scheduler's interleaving as much as the heuristic.\n"
                    "Read the ratio, not the absolute numbers.\n");
    }

    std::printf("\n%-14s %12s %12s %12s %12s %10s\n", "config", "attempts", "succeeded", "sticky hits",
                "continuations", "ms");

    // Best of several: a single run on a contended box says very little.
    sample best_off{SIZE_MAX, 0u, 0u, 0u, 0.0};
    sample best_on{SIZE_MAX, 0u, 0u, 0u, 0.0};

    for(int pass = 0; pass < 5; ++pass) {
        const auto off = measure(pool, false, stages, width);
        const auto on = measure(pool, true, stages, width);

        if(off.attempts < best_off.attempts) {
            best_off = off;
        }
        if(on.attempts < best_on.attempts) {
            best_on = on;
        }
    }

    report("random", best_off);
    report("sticky", best_on);

    if(best_off.attempts > 0u) {
        std::printf("\nattempts ratio (sticky / random): %.3f\n",
                    static_cast<double>(best_on.attempts) / static_cast<double>(best_off.attempts));
    }

    if(best_on.succeeded > 0u) {
        std::printf("success rate: random %.1f%%, sticky %.1f%%\n",
                    100.0 * static_cast<double>(best_off.succeeded) / static_cast<double>(best_off.attempts),
                    100.0 * static_cast<double>(best_on.succeeded) / static_cast<double>(best_on.attempts));
    }

    return 0;
}
