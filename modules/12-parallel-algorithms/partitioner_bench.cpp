// Module 12, exercise 1 -- three partitioners on three workload shapes.
//
// The exercise's claim is that each partitioner should win one of them. That is
// a prediction, and this is the file that checks it:
//
//   (a) uniform          every item costs the same
//   (b) proportional     item i costs i -- so the second half is 3x the first
//   (c) heavy tail       most items are cheap, a few are enormous
//
// A measurement, not a test. `nproc` is reported alongside, per docs/CLAUDE.md.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <random>
#include <thread>
#include <vector>

#include <acpp/algorithm.hpp>
#include <acpp/testing.hpp>

namespace {

constexpr std::size_t items = 4000u;

/** Burn a controlled amount of time. volatile so it cannot be optimised out. */
void burn(const int units) {
    volatile std::int64_t sink = 0;
    for(int i = 0; i < units * 400; ++i) {
        sink += i;
    }
}

std::vector<int> uniform_cost() {
    return std::vector<int>(items, 8);
}

std::vector<int> proportional_cost() {
    std::vector<int> cost(items);
    for(std::size_t i = 0u; i < items; ++i) {
        cost[i] = 1 + static_cast<int>((16u * i) / items);
    }
    return cost;
}

std::vector<int> heavy_tail_cost() {
    std::vector<int> cost(items, 1);
    std::mt19937 random{20260826u};
    // 2% of items carry most of the work, at unpredictable positions -- the
    // shape that makes a static split gamble on where they land.
    for(std::size_t i = 0u; i < items / 50u; ++i) {
        cost[random() % items] = 300;
    }
    return cost;
}

template<typename Partitioner>
[[nodiscard]] double run(acpp::executor &pool, const std::vector<int> &cost, const Partitioner &part) {
    acpp::taskflow graph;
    std::atomic<std::size_t> visited{0u};

    graph.emplace_runtime([&](acpp::runtime &rt) {
        acpp::for_each_index(
            rt, std::size_t{0u}, cost.size(),
            [&](const std::size_t i) {
                burn(cost[i]);
                visited.fetch_add(1u, std::memory_order_relaxed);
            },
            part);
    });

    const auto started = std::chrono::steady_clock::now();
    pool.run(graph)->wait();
    const auto finished = std::chrono::steady_clock::now();

    if(visited.load() != cost.size()) {
        std::printf("  !! visited %zu of %zu -- the measurement is not measuring what it claims\n",
                    visited.load(), cost.size());
    }

    return std::chrono::duration<double, std::milli>{finished - started}.count();
}

template<typename Partitioner>
[[nodiscard]] double best_of(acpp::executor &pool, const std::vector<int> &cost, const Partitioner &part,
                             const int passes = 3) {
    double best = 1e300;

    for(int pass = 0; pass < passes; ++pass) {
        const auto ms = run(pool, cost, part);
        best = ms < best ? ms : best;
    }

    return best;
}

} // namespace

int main() {
    acpp::executor pool{4u};

    std::printf("%zu items, %u workers, hardware_concurrency() = %u\n", items, pool.num_workers(),
                std::thread::hardware_concurrency());

    if(std::thread::hardware_concurrency() < 2u) {
        std::printf("ONE CORE: the total work is identical across partitioners, so what is\n"
                    "being compared here is scheduling overhead and imbalance, NOT speedup.\n"
                    "Load imbalance costs nothing when there is only one core to be idle.\n");
    }

    std::printf("\n%-16s %10s %10s %10s %10s\n", "workload", "static", "dynamic", "guided", "random");

    const struct {
        const char *name;
        std::vector<int> cost;
    } workloads[] = {
        {"uniform", uniform_cost()},
        {"proportional", proportional_cost()},
        {"heavy tail", heavy_tail_cost()},
    };

    for(const auto &workload: workloads) {
        const auto s = best_of(pool, workload.cost, acpp::static_partitioner<>{});
        const auto d = best_of(pool, workload.cost, acpp::dynamic_partitioner<>{64u});
        const auto g = best_of(pool, workload.cost, acpp::guided_partitioner<>{8u});
        const auto r = best_of(pool, workload.cost, acpp::random_partitioner<>{});

        std::printf("%-16s %10.2f %10.2f %10.2f %10.2f\n", workload.name, s, d, g, r);
    }

    std::printf("\nchunking behaviour (what the strategies actually do differently):\n");

    // The static partitioner's "auto" split, spelled out. This part is exact
    // and is not affected by the machine.
    const acpp::static_partitioner<> automatic;
    std::printf("  static, N=%zu W=4, auto chunk per worker: ", items);
    for(std::size_t w = 0u; w < 4u; ++w) {
        std::printf("%zu ", automatic.adjusted_chunk_size(items, 4u, w));
    }
    std::printf("\n");

    const acpp::static_partitioner<> remainder;
    std::printf("  static, N=10 W=4, auto chunk per worker:  ");
    for(std::size_t w = 0u; w < 4u; ++w) {
        std::printf("%zu ", remainder.adjusted_chunk_size(10u, 4u, w));
    }
    std::printf("   <- the remainder goes one item at a time to the first N%%W\n");

    return 0;
}
