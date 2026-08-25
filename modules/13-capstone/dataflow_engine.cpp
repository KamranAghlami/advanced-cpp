// Capstone -- the reactive dataflow engine, checked.
//
// The claim being tested is not "it computes the right numbers" -- that would
// be true of a full recomputation too. It is:
//
//   setting a cell recomputes EXACTLY its transitive dependents, and nothing
//   else, and the answers are identical to a full recomputation.
//
// Both halves matter. A partial recomputation that is correct but recomputes
// everything is pointless; one that is minimal but wrong is worse than useless.

#include <atomic>
#include <cmath>
#include <cstdint>
#include <random>
#include <thread>
#include <vector>

#include <acpp/dataflow.hpp>
#include <acpp/testing.hpp>

namespace {

using acpp::cell_id;

// The static_assert battery the capstone deliverables ask for.
static_assert(acpp::handle_like<cell_id>, "cell_id must be a Module 5 handle");
static_assert(sizeof(cell_id) == sizeof(std::uint32_t), "a cell id is one word");
static_assert(acpp::handle_traits<cell_id>::index_bits == 20u, "20 bits of index: 1,048,575 cells");
static_assert(acpp::handle_traits<cell_id>::version_bits == 12u);
static_assert(std::is_enum_v<cell_id>, "a strong type, not a bare integer");

} // namespace

int main() {
    acpp::testing::suite suite{"capstone / dataflow_engine"};

    acpp::executor pool{4u};
    suite.note("hardware_concurrency() = %u, workers = %u", std::thread::hardware_concurrency(),
               pool.num_workers());

    // --- the basics ---------------------------------------------------------
    {
        acpp::dataflow flow;

        const auto a = flow.input(2.0);
        const auto b = flow.input(3.0);
        const auto sum = flow.compute({a, b}, [](const std::vector<double> &in) { return in[0] + in[1]; });
        const auto doubled = flow.compute({sum}, [](const std::vector<double> &in) { return in[0] * 2.0; });

        flow.recompute(pool);

        suite.check(flow.get(sum) == 5.0, "a + b");
        suite.check(flow.get(doubled) == 10.0, "and the cell that depends on it");

        flow.set(a, 10.0);
        flow.recompute(pool);

        suite.check(flow.get(sum) == 13.0, "changing an input propagates");
        suite.check(flow.get(doubled) == 26.0, "transitively");
    }

    // --- the actual claim: only the affected subgraph is recomputed ---------
    {
        acpp::dataflow flow;

        // Two independent chains from two inputs, so "only the dependents" has
        // something to be wrong about.
        const auto left_input = flow.input(1.0);
        const auto right_input = flow.input(1.0);

        std::vector<cell_id> left_chain;
        std::vector<cell_id> right_chain;

        auto left = left_input;
        auto right = right_input;

        for(int i = 0; i < 10; ++i) {
            left = flow.compute({left}, [](const std::vector<double> &in) { return in[0] + 1.0; });
            right = flow.compute({right}, [](const std::vector<double> &in) { return in[0] + 1.0; });
            left_chain.push_back(left);
            right_chain.push_back(right);
        }

        flow.recompute(pool);
        suite.check(flow.get(left) == 11.0 && flow.get(right) == 11.0, "both chains computed");

        // Touch one input. Exactly its 10 dependents should be dirty.
        flow.set(left_input, 100.0);

        suite.check(flow.dirty_count() == 10u, "exactly the 10 cells downstream of the touched input");

        bool left_dirty = true;
        for(const auto id: left_chain) {
            left_dirty = left_dirty && flow.is_dirty(id);
        }

        suite.check(left_dirty, "every cell in the touched chain is dirty");

        bool right_clean = true;
        for(const auto id: right_chain) {
            right_clean = right_clean && !flow.is_dirty(id);
        }

        suite.check(right_clean, "and nothing in the untouched chain");

        flow.recompute(pool);

        suite.check(flow.last_recomputed() == 10u, "so only 10 of 22 cells were recomputed");
        suite.check(flow.get(left) == 110.0, "the touched chain updated");
        suite.check(flow.get(right) == 11.0, "and the untouched one did not change");
    }

    // --- a diamond is visited once ------------------------------------------
    //
    // The failure this guards against: marking dirty by walking dependents
    // without a visited set is exponential in a diamond-shaped graph. The
    // sparse set's O(1) `contains` is the early-out.
    {
        acpp::dataflow flow;

        auto root = flow.input(1.0);
        std::vector<cell_id> level{root};

        // 12 diamond levels: 2^12 paths from root to the tip, 25 cells.
        for(int i = 0; i < 12; ++i) {
            const auto left = flow.compute({level.back()},
                                           [](const std::vector<double> &in) { return in[0] + 1.0; });
            const auto right = flow.compute({level.back()},
                                            [](const std::vector<double> &in) { return in[0] + 1.0; });
            const auto join = flow.compute({left, right},
                                           [](const std::vector<double> &in) { return (in[0] + in[1]) / 2.0; });
            level.push_back(join);
        }

        flow.recompute(pool);
        const auto tip = level.back();
        suite.check(flow.get(tip) == 13.0, "the diamond stack computed");

        flow.set(root, 100.0);
        suite.check(flow.dirty_count() == 36u, "12 levels x 3 cells, each marked once");

        flow.recompute(pool);
        suite.check(flow.get(tip) == 112.0, "and recomputed correctly through 4096 paths");
    }

    // --- partial equals full ------------------------------------------------
    //
    // The strongest available check: build a random DAG, drive it with random
    // updates, and compare the incremental engine against full recomputation at
    // every step.
    {
        constexpr int cells = 120;
        std::mt19937 random{20260826u};

        acpp::dataflow incremental;
        acpp::dataflow reference;

        std::vector<cell_id> inputs;
        std::vector<cell_id> all_a;
        std::vector<cell_id> all_b;

        for(int i = 0; i < 8; ++i) {
            const auto value = 1.0 + i;
            inputs.push_back(incremental.input(value));
            all_a.push_back(inputs.back());
            all_b.push_back(reference.input(value));
        }

        for(int i = 8; i < cells; ++i) {
            // Depend on two earlier cells, so the graph is a real DAG rather
            // than a chain, and cycles are impossible by construction.
            const auto first = static_cast<std::size_t>(random() % all_a.size());
            const auto second = static_cast<std::size_t>(random() % all_a.size());

            const auto rule = [](const std::vector<double> &in) {
                return std::fmod(in[0] * 1.5 + in[1] * 0.5, 1000.0);
            };

            all_a.push_back(incremental.compute({all_a[first], all_a[second]}, rule));
            all_b.push_back(reference.compute({all_b[first], all_b[second]}, rule));
        }

        incremental.recompute(pool);
        reference.recompute_all_serial();

        bool agree = true;
        std::size_t total_incremental = 0u;

        for(int step = 0; step < 60 && agree; ++step) {
            const auto which = static_cast<std::size_t>(random() % inputs.size());
            const auto value = static_cast<double>(random() % 1000u);

            incremental.set(inputs[which], value);
            reference.set(all_b[which], value);

            incremental.recompute(pool);
            total_incremental += incremental.last_recomputed();
            reference.recompute_all_serial();

            for(std::size_t i = 0u; i < all_a.size(); ++i) {
                if(std::abs(incremental.get(all_a[i]) - reference.get(all_b[i])) > 1e-9) {
                    agree = false;
                    break;
                }
            }
        }

        suite.check(agree, "incremental recomputation agrees with full recomputation over 60 updates");
        suite.note("incremental recomputed %zu cells across 60 updates; full would have been %d",
                   total_incremental, 60 * cells);
        suite.check(total_incremental < 60u * static_cast<std::size_t>(cells),
                    "and did strictly less work");
    }

    // --- no-op updates are free --------------------------------------------
    {
        acpp::dataflow flow;
        const auto a = flow.input(5.0);
        const auto b = flow.compute({a}, [](const std::vector<double> &in) { return in[0] * 3.0; });

        flow.recompute(pool);
        suite.check(flow.get(b) == 15.0, "computed");

        flow.set(a, 5.0); // same value
        suite.check(flow.dirty_count() == 0u, "setting a cell to its current value dirties nothing");

        flow.recompute(pool);
        suite.check(flow.last_recomputed() == 0u, "and recomputes nothing");
    }

    return suite.report();
}
