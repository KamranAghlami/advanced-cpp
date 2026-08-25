#ifndef ACPP_GRAPH_HPP
#define ACPP_GRAPH_HPP

// Module 11 -- how a task graph is represented.
//
// Four ideas, all of them about packing:
//
//   * heterogeneous node kinds in ONE type, via a variant, so there is no
//     inheritance and no per-node heap indirection;
//   * successors and predecessors in ONE vector, partitioned at an index;
//   * dependencies as atomic join counters, so there is no central scheduler
//     state and no lock;
//   * node state split by WHO TOUCHES IT -- one plain word for fields only the
//     owning thread reads, one atomic word for the rest, with a refcount packed
//     into the atomic's spare bits.

#include "config.hpp"
#include "small_vector.hpp"
#include "stl/atomic.hpp"
#include "stl/cstddef.hpp"
#include "stl/cstdint.hpp"
#include "stl/functional.hpp"
#include "stl/memory.hpp"
#include "stl/utility.hpp"
#include "stl/type_traits.hpp"
#include "stl/vector.hpp"

#include <exception>
#include <string>
#include <variant>

namespace acpp {

class node;
class runtime;

/**
 * Non-atomic node state: fields only ever touched by the thread that owns the
 * node at that moment. No atomics, because there is nothing to synchronise.
 */
namespace nstate {

using type = stl::uint32_t;

inline constexpr type none = 0u;
inline constexpr type conditioned = 1u << 0u;  //< a condition chose this edge
inline constexpr type detached = 1u << 1u;

// Priority lives in the same word, as a mask and a shift. Same trick as
// Module 5's handle, applied to scheduler metadata.
inline constexpr type priority_shift = 8u;
inline constexpr type priority_mask = 0x3u << priority_shift;

} // namespace nstate

/**
 * Atomic node state: fields written by one thread and read by another.
 *
 * The low 24 bits are a **reference count**, packed into the same word rather
 * than given an atomic of their own. One RMW instead of two, and no extra cache
 * line. Exactly the trick Taskflow uses for AsyncTask, and exactly what
 * Module 8's 24-byte `shallow_any` was missing.
 */
namespace estate {

using type = stl::uint32_t;

inline constexpr type none = 0u;
inline constexpr type refcount_mask = 0x00FFFFFFu;
inline constexpr type refcount_inc = 1u;
inline constexpr type exception = 1u << 24u;
inline constexpr type cancelled = 1u << 25u;

} // namespace estate

/** What a node actually does. */
struct static_task {
    stl::function<void()> work;
};

/** Returns the index of the single successor to schedule. */
struct condition_task {
    stl::function<int()> work;
};

/** Gets the runtime, so it can spawn and corun (Module 12). */
struct runtime_task {
    stl::function<void(runtime &)> work;
};

/** A task with no graph: scheduled directly, refcounted. */
struct async_task {
    stl::function<void()> work;
};

class node {
    friend class executor;
    friend class taskflow;
    friend class runtime;
    friend class task;

public:
    // The order here is the order of the switch in Executor::_invoke, and the
    // indices are part of that contract. sizeof(node) is the max over these
    // plus a discriminator -- see modules/11-scheduler/NOTES.md for which one
    // actually drives the size.
    using handle_type = std::variant<static_task, condition_task, runtime_task, async_task>;

    enum kind : stl::size_t {
        is_static = 0u,
        is_condition = 1u,
        is_runtime = 2u,
        is_async = 3u,
    };

    node() = default;

    template<typename... Args>
    explicit node(std::in_place_type_t<static_task>, Args &&...args)
        : handle{std::in_place_type<static_task>, stl::forward<Args>(args)...} {}

    template<typename Kind, typename... Args>
    node(std::in_place_type_t<Kind> tag, Args &&...args)
        : handle{tag, stl::forward<Args>(args)...} {}

    node(const node &) = delete;
    node &operator=(const node &) = delete;

    [[nodiscard]] stl::size_t num_successors() const noexcept { return successor_count; }
    [[nodiscard]] stl::size_t num_predecessors() const noexcept { return edges.size() - successor_count; }
    [[nodiscard]] const std::string &name() const noexcept { return label; }

    [[nodiscard]] node *successor(const stl::size_t pos) const noexcept { return edges[pos]; }
    [[nodiscard]] node *predecessor(const stl::size_t pos) const noexcept {
        return edges[successor_count + pos];
    }

    /**
     * Add an edge this -> other. O(1), one vector, one allocation at most.
     *
     * Push to the back, swap into the partition boundary, bump the boundary --
     * so successors occupy [0, successor_count) and predecessors the rest,
     * without a second container or a second allocation.
     */
    void precede(node &other) {
        edges.push_back(&other);
        stl::swap(edges[successor_count], edges[edges.size() - 1u]);
        ++successor_count;
        other.edges.push_back(this); // lands in the predecessor half by construction
    }

    /** Remove the edge this -> other, keeping both partitions intact. */
    bool remove_successor(node &other) {
        for(stl::size_t pos = 0u; pos < successor_count; ++pos) {
            if(edges[pos] == &other) {
                // Move the last successor into the hole, then move the first
                // predecessor into the vacated boundary slot. Two swaps, no
                // shifting, both partitions still contiguous.
                edges[pos] = edges[successor_count - 1u];
                edges[successor_count - 1u] = edges.back();
                edges.pop_back();
                --successor_count;

                other.remove_predecessor(*this);
                return true;
            }
        }

        return false;
    }

    [[nodiscard]] stl::size_t join_value() const noexcept {
        return join_counter.load(stl::memory_order_relaxed);
    }

    // --- packed refcount, in the spare bits of the atomic state word --------

    void retain() noexcept { estate_word.fetch_add(estate::refcount_inc, stl::memory_order_relaxed); }

    /** True when this call took the count to zero. */
    bool release() noexcept {
        const auto previous = estate_word.fetch_sub(estate::refcount_inc, stl::memory_order_acq_rel);
        return (previous & estate::refcount_mask) == 1u;
    }

    [[nodiscard]] stl::uint32_t use_count() const noexcept {
        return estate_word.load(stl::memory_order_relaxed) & estate::refcount_mask;
    }

    void mark(const estate::type flag) noexcept {
        estate_word.fetch_or(flag, stl::memory_order_release);
    }

    [[nodiscard]] bool marked(const estate::type flag) const noexcept {
        return (estate_word.load(stl::memory_order_acquire) & flag) != 0u;
    }

    [[nodiscard]] unsigned priority() const noexcept {
        return (nstate_word & nstate::priority_mask) >> nstate::priority_shift;
    }

    void set_priority(const unsigned value) noexcept {
        nstate_word = (nstate_word & ~nstate::priority_mask)
                      | ((value << nstate::priority_shift) & nstate::priority_mask);
    }

private:
    void remove_predecessor(node &other) {
        for(stl::size_t pos = successor_count; pos < edges.size(); ++pos) {
            if(edges[pos] == &other) {
                edges[pos] = edges.back();
                edges.pop_back();
                return;
            }
        }
    }

    /** Reset for a re-run: a graph that runs twice needs its counters back. */
    void reset_join_counter() noexcept {
        join_counter.store(num_predecessors(), stl::memory_order_relaxed);
    }

    handle_type handle{static_task{}};

    // ONE vector, partitioned at successor_count: successors below, predecessors
    // above. Four inline slots, because most nodes have at most four edges, so
    // most graphs allocate nothing for their topology.
    small_vector<node *, 4u> edges{};
    stl::size_t successor_count{0u};

    stl::atomic<stl::size_t> join_counter{0u};

    // Split by who touches them. Non-atomic for fields the owning thread alone
    // reads; atomic for the rest, with the refcount in its spare bits.
    nstate::type nstate_word{nstate::none};
    stl::atomic<estate::type> estate_word{estate::none};

    // Which run this node currently belongs to. Written once per run, before
    // any of it is scheduled, so it needs no synchronisation of its own.
    class topology *run{nullptr};

    // Async nodes only: the counter their completion decrements, and the flag
    // that says the executor owns this node and must delete it after running.
    // Null for every node that belongs to a taskflow.
    stl::atomic<stl::size_t> *async_counter{nullptr};

    std::string label{};
};

/** A handle to a node in a graph. Copyable, non-owning. */
class task {
public:
    task() = default;

    explicit task(node *target) noexcept
        : target{target} {}

    task &precede(const task &other) {
        target->precede(*other.target);
        return *this;
    }

    task &succeed(const task &other) {
        other.target->precede(*target);
        return *this;
    }

    task &name(std::string value) {
        target->label = stl::move(value);
        return *this;
    }

    task &priority(const unsigned value) {
        target->set_priority(value);
        return *this;
    }

    [[nodiscard]] node *raw() const noexcept { return target; }
    [[nodiscard]] explicit operator bool() const noexcept { return target != nullptr; }

private:
    node *target{nullptr};
};

/** A graph. Owns its nodes; nodes never move, so `node *` edges stay valid. */
class taskflow {
    friend class executor;

public:
    taskflow() = default;

    taskflow(const taskflow &) = delete;
    taskflow &operator=(const taskflow &) = delete;

    taskflow(taskflow &&) = default;
    taskflow &operator=(taskflow &&) = default;

    template<typename Fn>
        requires stl::is_invocable_v<Fn>
    task emplace(Fn &&work) {
        nodes.push_back(stl::make_unique<node>(std::in_place_type<static_task>,
                                               stl::function<void()>{stl::forward<Fn>(work)}));
        return task{nodes.back().get()};
    }

    /** A condition task returns the index of the one successor to schedule. */
    template<typename Fn>
        requires stl::is_invocable_r_v<int, Fn>
    task emplace_condition(Fn &&work) {
        nodes.push_back(stl::make_unique<node>(std::in_place_type<condition_task>,
                                               stl::function<int()>{stl::forward<Fn>(work)}));
        return task{nodes.back().get()};
    }

    /** A runtime task gets the scheduler, for nested work and corun. */
    template<typename Fn>
        requires stl::is_invocable_v<Fn, runtime &>
    task emplace_runtime(Fn &&work) {
        nodes.push_back(stl::make_unique<node>(std::in_place_type<runtime_task>,
                                               stl::function<void(runtime &)>{stl::forward<Fn>(work)}));
        return task{nodes.back().get()};
    }

    [[nodiscard]] stl::size_t size() const noexcept { return nodes.size(); }
    [[nodiscard]] bool empty() const noexcept { return nodes.empty(); }

    void clear() { nodes.clear(); }

private:
    // unique_ptr per node, not a vector<node>: edges are raw pointers, and a
    // vector reallocation would invalidate every one of them. The same pointer
    // stability question Module 7 answered with paging, answered differently
    // because the access pattern is different -- nodes are chased, not scanned.
    stl::vector<stl::unique_ptr<node>> nodes;
};

} // namespace acpp

#endif // ACPP_GRAPH_HPP
