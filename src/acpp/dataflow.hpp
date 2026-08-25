#ifndef ACPP_DATAFLOW_HPP
#define ACPP_DATAFLOW_HPP

// Capstone (Option B) -- a reactive dataflow engine.
//
// Values are cells, dependencies are a DAG, and recomputation is a PARTIAL
// topological execution: setting a cell dirties only its transitive dependents,
// and only those are recomputed.
//
// What it is built out of, and why each piece is there:
//
//   Module 5   generational handles      cell ids that detect staleness
//   Module 6   sparse set                O(1) dirty-set membership, and dense
//                                        iteration over exactly the dirty cells
//   Module 9   work-stealing deque       per-worker queues in the executor
//   Module 10  two-phase notifier        workers sleep between recomputes
//   Module 11  graph + executor          the partial topological execution
//
// The design decisions and their rejected alternatives are in docs/design.md.

#include "config.hpp"
#include "executor.hpp"
#include "graph.hpp"
#include "handle.hpp"
#include "sparse_set.hpp"
#include "stl/cstddef.hpp"
#include "stl/cstdint.hpp"
#include "stl/functional.hpp"
#include "stl/utility.hpp"
#include "stl/vector.hpp"

namespace acpp {

/** A cell handle. An enum over uint32_t, so Module 5's traits apply unchanged. */
enum class cell_id : stl::uint32_t {};

class dataflow {
    using traits_type = handle_traits<cell_id>;

    struct cell {
        double value{0.0};
        // Empty for an input cell. That is the discriminator: an input has no
        // rule, a computed cell has one, and there is no third kind.
        stl::function<double(const stl::vector<double> &)> rule{};
        stl::vector<cell_id> inputs{};
        stl::vector<cell_id> dependents{};
    };

public:
    dataflow()
        : dirty{deletion_policy::swap_and_pop} {}

    /** A cell with no rule. Only `set` changes it. */
    [[nodiscard]] cell_id input(const double initial = 0.0) {
        const auto id = traits_type::construct(static_cast<stl::uint32_t>(cells.size()), 0u);
        cells.push_back(cell{initial, {}, {}, {}});
        return id;
    }

    /** A cell computed from others. Edges are recorded in both directions. */
    template<typename Fn>
    [[nodiscard]] cell_id compute(stl::vector<cell_id> inputs, Fn rule) {
        const auto id = traits_type::construct(static_cast<stl::uint32_t>(cells.size()), 0u);

        cells.push_back(cell{0.0, stl::function<double(const stl::vector<double> &)>{stl::move(rule)},
                             inputs, {}});

        for(const auto dependency: inputs) {
            cells[index_of(dependency)].dependents.push_back(id);
        }

        // A newly created cell has never been evaluated, so it is dirty by
        // definition. Not marking it here is the easiest way to ship a value
        // that is silently zero.
        mark_dirty(id);
        return id;
    }

    [[nodiscard]] double get(const cell_id id) const noexcept { return cells[index_of(id)].value; }

    [[nodiscard]] stl::size_t size() const noexcept { return cells.size(); }
    [[nodiscard]] stl::size_t dirty_count() const noexcept { return dirty.count(); }
    [[nodiscard]] stl::size_t last_recomputed() const noexcept { return recomputed; }

    /**
     * Change an input and dirty its transitive dependents.
     *
     * Not thread-safe against `recompute` or against another `set` -- see
     * docs/design.md for why that is the contract rather than a limitation.
     */
    void set(const cell_id id, const double value) {
        auto &target = cells[index_of(id)];

        // Nothing changed, nothing to invalidate. Cheap, and it is the case
        // that makes a reactive system usable: most updates are no-ops.
        if(target.value == value && !dirty.contains(id)) {
            return;
        }

        target.value = value;

        for(const auto dependent: target.dependents) {
            mark_dirty_transitively(dependent);
        }
    }

    /** Recompute exactly the dirty cells, in dependency order, in parallel. */
    void recompute(executor &pool) {
        recomputed = 0u;

        if(dirty.empty()) {
            return;
        }

        // One node per dirty cell. Building the graph each time is a real cost
        // and a deliberate one -- see docs/design.md for the rejected
        // alternative (a cached graph invalidated on topology change).
        taskflow graph;
        stl::vector<task> tasks;
        tasks.reserve(dirty.size());

        // The sparse set gives dense iteration over exactly the dirty cells and
        // O(1) membership for the edge filter below. A std::set would give the
        // second and not the first.
        stl::vector<cell_id> pending;
        pending.reserve(dirty.size());

        for(const auto id: dirty) {
            pending.push_back(id);
        }

        // A rule reads its inputs' *values*, which are only correct once those
        // inputs have run. So the closure captures pointers and reads at
        // execution time, not at build time -- the edges below are what make
        // that ordering true.
        for(const auto id: pending) {
            auto *target = &cells[index_of(id)];
            auto *self = this;

            tasks.push_back(graph.emplace([target, self] {
                if(!target->rule) {
                    return;
                }

                stl::vector<double> arguments;
                arguments.reserve(target->inputs.size());

                for(const auto input: target->inputs) {
                    arguments.push_back(self->cells[index_of(input)].value);
                }

                target->value = target->rule(arguments);
            }));
        }

        // Edges only between cells that are BOTH dirty. A clean input is
        // already correct, so waiting on it would be a dependency on nothing --
        // and would drag the whole graph back into the recomputation.
        for(stl::size_t pos = 0u; pos < pending.size(); ++pos) {
            for(const auto input: cells[index_of(pending[pos])].inputs) {
                if(dirty.contains(input)) {
                    tasks[position_of(pending, input)].precede(tasks[pos]);
                }
            }
        }

        pool.run(graph)->wait();

        recomputed = pending.size();
        dirty.clear();
    }

    /**
     * Incremental, but serial: recompute exactly the dirty cells with no
     * executor at all.
     *
     * The third leg of the comparison, and the one that separates the two
     * effects the design mixes together -- WORK AVOIDED (this versus
     * recompute_all_serial) and PARALLEL EXECUTION (this versus recompute).
     * Without it a benchmark cannot say which of the two it is measuring.
     *
     * Cells are created in dependency order, so creation order is a valid
     * topological order and no sorting is needed.
     */
    void recompute_serial() {
        recomputed = 0u;

        for(stl::size_t pos = 0u; pos < cells.size(); ++pos) {
            const auto id = traits_type::construct(static_cast<stl::uint32_t>(pos), 0u);

            if(!dirty.contains(id)) {
                continue;
            }

            ++recomputed;
            auto &target = cells[pos];

            if(!target.rule) {
                continue;
            }

            stl::vector<double> arguments;
            arguments.reserve(target.inputs.size());

            for(const auto input: target.inputs) {
                arguments.push_back(cells[index_of(input)].value);
            }

            target.value = target.rule(arguments);
        }

        dirty.clear();
    }

    /** The comparison baseline: recompute everything, in order. */
    void recompute_all_serial() {
        for(stl::size_t pos = 0u; pos < cells.size(); ++pos) {
            auto &target = cells[pos];

            if(!target.rule) {
                continue;
            }

            stl::vector<double> arguments;
            arguments.reserve(target.inputs.size());

            for(const auto input: target.inputs) {
                arguments.push_back(cells[index_of(input)].value);
            }

            target.value = target.rule(arguments);
        }

        recomputed = cells.size();
        dirty.clear();
    }

    [[nodiscard]] bool is_dirty(const cell_id id) const noexcept { return dirty.contains(id); }

private:
    [[nodiscard]] static stl::size_t index_of(const cell_id id) noexcept {
        return static_cast<stl::size_t>(traits_type::to_index(id));
    }

    [[nodiscard]] static stl::size_t position_of(const stl::vector<cell_id> &order, const cell_id id) {
        for(stl::size_t pos = 0u; pos < order.size(); ++pos) {
            if(order[pos] == id) {
                return pos;
            }
        }

        return 0u;
    }

    void mark_dirty(const cell_id id) { dirty.push(id); }

    /**
     * Depth-first, with the sparse set as the visited set.
     *
     * `contains` is the early-out that makes this linear in the *newly* dirtied
     * subgraph rather than exponential in a diamond-shaped one -- a cell reached
     * by two paths is visited once.
     */
    void mark_dirty_transitively(const cell_id id) {
        if(dirty.contains(id)) {
            return;
        }

        dirty.push(id);

        for(const auto dependent: cells[index_of(id)].dependents) {
            mark_dirty_transitively(dependent);
        }
    }

    stl::vector<cell> cells;
    basic_sparse_set<cell_id> dirty;
    stl::size_t recomputed{0u};
};

} // namespace acpp

#endif // ACPP_DATAFLOW_HPP
