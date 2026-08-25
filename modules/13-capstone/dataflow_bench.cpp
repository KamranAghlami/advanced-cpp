// Capstone -- incremental recomputation against full recomputation.
//
// The claim under test: touching one input in a large graph should cost
// proportional to the *affected subgraph*, not to the graph.
//
// A measurement, not a test. `nproc` is reported, per docs/CLAUDE.md.

#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>
#include <thread>
#include <vector>

#include <acpp/dataflow.hpp>

namespace {

struct built {
    std::vector<acpp::cell_id> inputs;
    std::vector<acpp::cell_id> all;
};

/**
 * A layered DAG: `width` cells per layer, each depending on two cells from the
 * layer below. Layered rather than random so the affected fraction of a single
 * input is predictable -- which is what makes the ratio interpretable.
 */
[[nodiscard]] built build(acpp::dataflow &flow, const int layers, const int width) {
    built result;
    std::mt19937 random{20260826u};
    std::vector<acpp::cell_id> previous;

    for(int i = 0; i < width; ++i) {
        result.inputs.push_back(flow.input(1.0 + i));
        previous.push_back(result.inputs.back());
        result.all.push_back(result.inputs.back());
    }

    for(int layer = 1; layer < layers; ++layer) {
        std::vector<acpp::cell_id> current;

        for(int i = 0; i < width; ++i) {
            // One parent round-robin, one random. The round-robin half
            // guarantees every cell in the previous layer has at least one
            // dependent -- without it, some inputs are dead ends and touching
            // them dirties nothing, which silently turns a measurement pass
            // into a measurement of nothing.
            const auto first = previous[static_cast<std::size_t>(i) % previous.size()];
            const auto second = previous[static_cast<std::size_t>(random() % previous.size())];

            current.push_back(flow.compute({first, second}, [](const std::vector<double> &in) {
                // Enough arithmetic that the comparison is about work rather
                // than about scheduling overhead. With a trivial rule the
                // incremental version LOSES, because building a task graph
                // costs more than evaluating the whole thing serially --
                // which is itself worth knowing and is reported below.
                double value = in[0] * 1.0009 + in[1] * 0.9991;
                for(int spin = 0; spin < 200; ++spin) {
                    value = std::fmod(value * 1.0000001 + 1.0, 1e6);
                }
                return value;
            }));
            result.all.push_back(current.back());
        }

        previous = current;
    }

    return result;
}

template<typename Fn>
[[nodiscard]] double time_ms(Fn &&fn) {
    const auto started = std::chrono::steady_clock::now();
    fn();
    const auto finished = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>{finished - started}.count();
}

} // namespace

int main() {
    acpp::executor pool{4u};

    std::printf("hardware_concurrency() = %u, workers = %u\n", std::thread::hardware_concurrency(),
                pool.num_workers());
    std::printf("\n%-8s %6s %8s %12s %12s %12s %11s\n", "layers", "width", "cells",
                "incr-par", "incr-ser", "full-ser", "recomputed");

    for(const auto &shape: {std::pair{6, 40}, std::pair{12, 40}, std::pair{20, 60}}) {
        const auto [layers, width] = shape;

        acpp::dataflow flow;
        const auto graph = build(flow, layers, width);
        flow.recompute(pool);

        std::mt19937 random{7u};

        // Best of several. Every pass must set a value the cell does not
        // already hold -- an unchanged value is correctly a no-op, and a
        // no-op pass would win the minimum and turn this into a benchmark of
        // nothing. The first version did exactly that.
        double best_incremental = 1e300;
        double best_incremental_serial = 1e300;
        double best_full = 1e300;
        std::size_t recomputed = 0u;
        double next_value = 1.0;

        for(int pass = 0; pass < 5; ++pass) {
            const auto which = graph.inputs[random() % graph.inputs.size()];

            next_value += 1.0;
            flow.set(which, next_value);

            if(flow.dirty_count() == 0u) {
                std::printf("  !! nothing was dirtied -- this pass measures nothing\n");
                continue;
            }

            const auto incremental = time_ms([&] { flow.recompute(pool); });
            recomputed = flow.last_recomputed();

            // Re-dirty the same subgraph so the serial incremental leg does
            // the same work, then measure it.
            next_value += 1.0;
            flow.set(which, next_value);
            const auto incremental_serial = time_ms([&] { flow.recompute_serial(); });

            const auto full = time_ms([&] { flow.recompute_all_serial(); });

            best_incremental = incremental < best_incremental ? incremental : best_incremental;
            best_incremental_serial =
                incremental_serial < best_incremental_serial ? incremental_serial : best_incremental_serial;
            best_full = full < best_full ? full : best_full;
        }

        std::printf("%-8d %6d %8zu %12.3f %12.3f %12.3f %11zu\n", layers, width, flow.size(),
                    best_incremental, best_incremental_serial, best_full, recomputed);
    }

    std::printf("\nThe `recomputed` column is the honest one: it is the size of the affected\n"
                "subgraph, and it is what the design actually optimises. It is also the only\n"
                "machine-independent number here.\n\n"
                "incr-ser vs full-ser isolates WORK AVOIDED.\n"
                "incr-par vs incr-ser isolates PARALLEL EXECUTION -- and on one core that is\n"
                "pure overhead, since incr-par also pays to build a task graph per recompute.\n");

    return 0;
}
