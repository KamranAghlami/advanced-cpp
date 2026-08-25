#ifndef ACPP_EXECUTOR_HPP
#define ACPP_EXECUTOR_HPP

// Module 11 -- the scheduler, assembled from everything Phase C has built.
//
//   Module 9's work-stealing deque   per-worker task queues
//   Module 10's notifier             sleeping without lost wakeups
//   Module 11's graph                variant nodes and atomic join counters
//
// Plus the three scheduler techniques that only appear once those exist:
//
//   * the CONTINUATION CACHE -- a finishing task with exactly one ready
//     successor runs it directly, with no queue round-trip;
//   * STICKY VICTIM -- after a successful steal, try the same victim first;
//   * cross-thread EXCEPTION PROPAGATION, with a documented answer for the
//     case where several tasks throw at once.

#include "config.hpp"
#include "graph.hpp"
#include "notifier.hpp"
#include "stl/atomic.hpp"
#include "stl/functional.hpp"
#include "stl/cstddef.hpp"
#include "stl/utility.hpp"
#include "stl/vector.hpp"
#include "wsq.hpp"

#include <chrono>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <random>
#include <thread>

namespace acpp {

class executor;

/** One run of one graph. */
class topology {
    friend class executor;

public:
    topology() = default;

    /** Blocks until the run finishes, then rethrows anything it caught. */
    void wait() {
        std::unique_lock guard{mutex};
        cv.wait(guard, [this] { return finished; });

        if(captured) {
            // Rethrown on the *calling* thread, once, after the graph is
            // quiescent. Rethrowing from a worker would unwind a worker.
            auto error = captured;
            captured = nullptr;
            std::rethrow_exception(error);
        }
    }

    [[nodiscard]] bool cancelled() const noexcept { return cancel.load(stl::memory_order_acquire); }

    /** How many tasks threw. Only the first is rethrown -- see NOTES.md. */
    [[nodiscard]] stl::size_t exception_count() const noexcept {
        return exceptions.load(stl::memory_order_acquire);
    }

private:
    stl::atomic<stl::size_t> pending{0u};
    stl::atomic<bool> cancel{false};
    stl::atomic<stl::size_t> exceptions{0u};

    std::mutex mutex;
    std::condition_variable cv;
    bool finished{false};
    std::exception_ptr captured{nullptr};
};

/** Handed to a runtime_task so it can schedule work and cooperate (Module 12). */
class runtime {
    friend class executor;

public:
    [[nodiscard]] executor &owner() const noexcept { return *host; }

    /** The worker that created this runtime -- informational only. The work it
     *  spawns may run anywhere, which is why corun does not use this. */
    [[nodiscard]] unsigned worker_id() const noexcept { return id; }

    /** Run a callable in this worker's own queue, tracked by a local counter. */
    template<typename Fn>
    void silent_async(Fn &&work);

    /**
     * Block until everything this runtime spawned has finished -- by JOINING IN
     * rather than by sleeping.
     *
     * This is the answer to the recursion problem. If a task calls wait() on
     * nested work from inside a worker, that worker blocks and the pool loses a
     * thread; do it on every worker and the pool deadlocks with work pending.
     * corun() instead re-enters the scheduling loop with a completion
     * predicate, so the blocked worker spends the wait executing the very work
     * it is waiting for.
     */
    void corun();

    /** corun() until an arbitrary predicate holds. */
    template<typename Predicate>
    void corun_until(Predicate &&stop);

private:
    runtime(executor *host, const unsigned id) noexcept
        : host{host}, id{id} {}

    // Non-copyable: the counter below is what corun() waits on, and a copy
    // would wait on the wrong one.
    runtime(const runtime &) = delete;
    runtime &operator=(const runtime &) = delete;

    executor *host;
    unsigned id;
    stl::atomic<stl::size_t> spawned{0u};
};

class executor {
    friend class runtime;

    struct worker {
        unsigned id{};
        bounded_wsq<node *, 8u> queue{};
        std::mt19937 random{};
        // Producer/consumer relationships in a task graph are stable, so after
        // a successful steal the same victim is likely to have more. Cheap
        // heuristic, large effect -- measured in modules/11-scheduler.
        unsigned sticky_victim{invalid_victim};

        // Diagnostics, and deliberately relaxed atomics rather than plain
        // integers. Each is written by exactly one worker and read by whoever
        // calls stats() or reset_stats() -- so a plain int is a data race, which
        // TSan duly reported. Making them atomic removes the *race* without
        // adding any ordering: a statistic may be observed slightly stale, and
        // that is the correct contract for a counter nobody schedules on.
        //
        // Single writer, so a load/store pair is enough; no RMW.
        stl::atomic<stl::size_t> steal_attempts{0u};
        stl::atomic<stl::size_t> steals_succeeded{0u};
        stl::atomic<stl::size_t> sticky_hits{0u};
        stl::atomic<stl::size_t> continuations{0u};

        static void bump(stl::atomic<stl::size_t> &counter) noexcept {
            counter.store(counter.load(stl::memory_order_relaxed) + 1u, stl::memory_order_relaxed);
        }
    };

    static constexpr unsigned invalid_victim = static_cast<unsigned>(-1);

    // Bounded, then yield, then a hard cap. Never spin forever: a worker that
    // spins is a worker burning a core somebody else could use.
    static constexpr stl::size_t max_steals = 32u;
    static constexpr stl::size_t hard_cap = 150u + max_steals;

public:
    explicit executor(unsigned count = 0u)
        : workers(count == 0u ? default_worker_count() : count),
          overflow{10u},
          notifier{static_cast<stl::size_t>(workers.size())} {
        for(unsigned id = 0u; id < workers.size(); ++id) {
            workers[id].id = id;
            workers[id].random.seed(id * 2654435761u + 1u);
        }

        threads.reserve(workers.size());

        for(unsigned id = 0u; id < workers.size(); ++id) {
            threads.emplace_back([this, id] { spin(id); });
        }
    }

    executor(const executor &) = delete;
    executor &operator=(const executor &) = delete;

    ~executor() {
        done.store(true, stl::memory_order_release);
        notifier.notify_all();

        for(auto &thread: threads) {
            thread.join();
        }
    }

    [[nodiscard]] unsigned num_workers() const noexcept { return static_cast<unsigned>(workers.size()); }

    /** Schedule a graph. The returned topology is the handle to wait on. */
    [[nodiscard]] stl::unique_ptr<topology> run(taskflow &graph) {
        auto handle = stl::make_unique<topology>();

        if(graph.nodes.empty()) {
            complete(*handle);
            return handle;
        }

        for(auto &owned: graph.nodes) {
            owned->reset_join_counter();
            owned->estate_word.store(estate::none, stl::memory_order_relaxed);
            owned->nstate_word &= ~nstate::conditioned;
            owned->run = handle.get();
        }

        // Sources: everything with no predecessors.
        stl::vector<node *> sources;
        for(auto &owned: graph.nodes) {
            if(owned->num_predecessors() == 0u) {
                sources.push_back(owned.get());
            }
        }

        if(sources.empty()) {
            // A graph with no source is a cycle. Nothing can ever run, so
            // saying so beats hanging.
            handle->cancel.store(true, stl::memory_order_release);
            handle->pending.store(0u, stl::memory_order_relaxed);
            complete(*handle);
            return handle;
        }

        // `pending` counts SCHEDULED nodes, not total nodes, and that is not a
        // detail. A condition task takes one branch and leaves the others
        // unrun, so "every node has finished" is never true for a graph with a
        // condition in it -- a static count would hang on exactly the graphs
        // conditions exist for. Found by the conditions test hanging.
        //
        // The counter is raised for a successor before its predecessor's own
        // decrement, so it can never transiently reach zero mid-graph.
        handle->pending.store(sources.size(), stl::memory_order_relaxed);

        for(auto *source: sources) {
            schedule(source);
        }

        return handle;
    }

    /** Diagnostics for the exercises. Not part of the scheduling contract. */
    struct statistics {
        stl::size_t steal_attempts{};
        stl::size_t steals_succeeded{};
        stl::size_t sticky_hits{};
        stl::size_t continuations{};
    };

    [[nodiscard]] statistics stats() const noexcept {
        statistics total;

        for(const auto &current: workers) {
            total.steal_attempts += current.steal_attempts.load(stl::memory_order_relaxed);
            total.steals_succeeded += current.steals_succeeded.load(stl::memory_order_relaxed);
            total.sticky_hits += current.sticky_hits.load(stl::memory_order_relaxed);
            total.continuations += current.continuations.load(stl::memory_order_relaxed);
        }

        return total;
    }

    void reset_stats() noexcept {
        for(auto &current: workers) {
            current.steal_attempts.store(0u, stl::memory_order_relaxed);
            current.steals_succeeded.store(0u, stl::memory_order_relaxed);
            current.sticky_hits.store(0u, stl::memory_order_relaxed);
            current.continuations.store(0u, stl::memory_order_relaxed);
        }
    }

    /** Turn the sticky-victim heuristic off, so the exercise can measure it. */
    void set_sticky(const bool value) noexcept { sticky_enabled = value; }

    /**
     * Run a callable on the pool, tracked by `counter`, with no graph involved.
     *
     * The node is heap-allocated and owned by the executor, which deletes it
     * after invoking. That is the price of a task with no taskflow to live in.
     */
    void spawn_async(stl::function<void()> work, stl::atomic<stl::size_t> &counter) {
        counter.fetch_add(1u, stl::memory_order_acq_rel);

        auto *task = new node{std::in_place_type<async_task>, stl::move(work)};
        task->async_counter = &counter;
        schedule(task);
    }

    /**
     * Participate in the pool until `stop()` returns true.
     *
     * Uses the CALLING thread's worker, not a worker id captured earlier.
     *
     * That distinction is the whole bug TSan found here: a `runtime` records
     * the worker that created it, but the work it spawns can run on any worker,
     * and that work can itself call corun. Keying off the stored id then had
     * two threads driving one worker's deque and one worker's RNG. The identity
     * that matters is "which thread am I", and thread_local is what answers it.
     */
    template<typename Predicate>
    void corun_until(Predicate &&stop) {
        auto *current = current_worker;

        if(current == nullptr) {
            // Called from outside the pool. Nothing to help with; just wait.
            while(!stop()) {
                std::this_thread::yield();
            }

            return;
        }

        auto &self = *current;
        unsigned idle = 0u;

        while(!stop()) {
            node *task = self.queue.pop();

            if(task == nullptr) {
                task = steal_once(self);
            }

            if(task != nullptr) {
                invoke(self, task);
                idle = 0u;
                continue;
            }

            // Nothing to help with. Yield first, then back off to a short
            // bounded sleep.
            //
            // A pure yield loop here is a busy-wait, and with more waiting
            // lines than cores it burns the machine: a 4-line pipeline on one
            // core spends three quarters of its time spinning. Found by
            // pipeline_ordering failing to finish in 500 s under TSan.
            //
            // The sleep is BOUNDED, which is what keeps it out of Module 10's
            // territory. Parking indefinitely here is the deadlock corun exists
            // to avoid; sleeping 50 us and re-checking is not -- progress does
            // not depend on anyone waking us.
            ++idle;

            if(idle < 64u) {
                std::this_thread::yield();
            } else {
                std::this_thread::sleep_for(std::chrono::microseconds{50});
            }
        }
    }

private:
    [[nodiscard]] static unsigned default_worker_count() noexcept {
        const auto hinted = std::thread::hardware_concurrency();
        // At least two: the victim-sampling trick below maps a draw over
        // [0, N-1) onto [0, N) minus self, which is only total for N >= 2.
        return hinted < 2u ? 2u : hinted;
    }

    void schedule(node *target) {
        if(auto *self = current_worker; self != nullptr && self->id < workers.size()) {
            if(workers[self->id].queue.try_push(target)) {
                notifier.notify_one();
                return;
            }
        }

        {
            const std::lock_guard guard{overflow_mutex};
            overflow.push(target);
        }

        notifier.notify_one();
    }

    void spin(const unsigned id) {
        auto &self = workers[id];
        current_worker = &self;

        node *task = nullptr;

        for(;;) {
            // 1. own queue first: LIFO, cache-warm.
            task = self.queue.pop();

            // 2. then steal.
            if(task == nullptr) {
                task = explore(self);
            }

            if(task != nullptr) {
                invoke(self, task);
                continue;
            }

            // 3. nothing anywhere. Two-phase wait.
            notifier.prepare_wait(id);

            if(has_any_work()) {
                notifier.cancel_wait(id);
                continue;
            }

            if(done.load(stl::memory_order_acquire)) {
                notifier.cancel_wait(id);
                current_worker = nullptr;
                return;
            }

            notifier.commit_wait(id);
        }
    }

    [[nodiscard]] bool has_any_work() {
        {
            const std::lock_guard guard{overflow_mutex};
            if(!overflow.empty()) {
                return true;
            }
        }

        for(auto &candidate: workers) {
            if(!candidate.queue.empty()) {
                return true;
            }
        }

        return false;
    }

    /** One steal attempt, for corun. No bounded-retry loop, no giving up. */
    [[nodiscard]] node *steal_once(worker &self) {
        {
            const std::lock_guard guard{overflow_mutex};
            if(auto *task = overflow.pop(); task != nullptr) {
                return task;
            }
        }

        worker::bump(self.steal_attempts);
        const auto victim = pick_victim(self);

        if(victim != self.id) {
            if(auto *task = workers[victim].queue.steal(); task != nullptr) {
                worker::bump(self.steals_succeeded);
                return task;
            }
        }

        return nullptr;
    }

    [[nodiscard]] node *explore(worker &self) {
        stl::size_t attempts = 0u;

        while(attempts < hard_cap) {
            worker::bump(self.steal_attempts);
            ++attempts;

            // The overflow queue first: it holds work that did not fit anywhere.
            {
                const std::lock_guard guard{overflow_mutex};
                if(auto *task = overflow.pop(); task != nullptr) {
                    return task;
                }
            }

            const auto victim = pick_victim(self);

            if(victim != self.id) {
                if(auto *task = workers[victim].queue.steal(); task != nullptr) {
                    worker::bump(self.steals_succeeded);

                    if(sticky_enabled) {
                        if(self.sticky_victim == victim) {
                            worker::bump(self.sticky_hits);
                        }
                        self.sticky_victim = victim;
                    }

                    return task;
                }

                if(self.sticky_victim == victim) {
                    self.sticky_victim = invalid_victim; // it dried up
                }
            }

            if(attempts >= max_steals) {
                std::this_thread::yield();
            }

            if(done.load(stl::memory_order_relaxed) && !has_any_work()) {
                return nullptr;
            }
        }

        return nullptr;
    }

    /**
     * A victim that is not us, with no rejection loop.
     *
     * Draw over [0, N-1) and shift past self: one modulo and one predicated
     * increment, and it is total because the constructor guarantees N >= 2.
     * Rejection sampling ("draw again if you got yourself") is the obvious
     * approach and has unbounded worst-case latency.
     *
     * (`%` over a non-power-of-two range has modulo bias, so "uniform" is
     * approximate. Negligible here, but it is being waved away knowingly.)
     */
    [[nodiscard]] unsigned pick_victim(worker &self) {
        if(sticky_enabled && self.sticky_victim != invalid_victim && self.sticky_victim != self.id) {
            return self.sticky_victim;
        }

        const auto count = static_cast<unsigned>(workers.size());
        auto victim = static_cast<unsigned>(self.random() % (count - 1u));

        if(victim >= self.id) {
            ++victim;
        }

        return victim;
    }

    void invoke(worker &self, node *target) {
        // The continuation cache. When a finishing task has exactly one ready
        // successor, the worker runs it DIRECTLY: no push, no pop, no chance of
        // another worker stealing a task that is already hot in this core's
        // cache. Queue traffic then happens only at fan-out, which is a large
        // part of why linear chains of small tasks stay fast.
        node *cache = nullptr;

    begin_invoke:
        auto *run = target->run;

        if(run != nullptr && run->cancel.load(stl::memory_order_acquire)) {
            finish_node(*run);
            if(cache != nullptr) {
                target = cache;
                cache = nullptr;
                worker::bump(self.continuations);
                goto begin_invoke;
            }
            return;
        }

        int chosen = -1;

        try {
            // switch on index(), not std::visit. Each case calls a dedicated
            // path, which keeps the per-kind logic in separately optimisable
            // code rather than one visitor body. Whether that beats std::visit
            // in codegen is a question for a compiler, not for folklore --
            // modules/11-scheduler/variant_dispatch_codegen.cpp asks it.
            switch(target->handle.index()) {
            case node::is_static:
                std::get_if<static_task>(&target->handle)->work();
                break;

            case node::is_condition:
                chosen = std::get_if<condition_task>(&target->handle)->work();
                break;

            case node::is_runtime: {
                runtime rt{this, self.id};
                std::get_if<runtime_task>(&target->handle)->work(rt);
                // Implicit join: a runtime task does not finish until the work
                // it spawned has. Doing it here rather than making the user
                // call corun() means forgetting to join is not a thing you can
                // do -- and forgetting is the whole failure mode.
                rt.corun();
                break;
            }

            case node::is_async:
                std::get_if<async_task>(&target->handle)->work();
                break;

            default:
                break;
            }
        } catch(...) {
            capture(run, target);
        }

        cache = nullptr;

        if(run == nullptr || !run->cancel.load(stl::memory_order_acquire)) {
            cache = release_successors(target, chosen);
        }

        // Read everything needed from `target` BEFORE finishing the node.
        //
        // finish_node may drive the topology's counter to zero, which releases
        // wait() on another thread -- and that thread may then destroy the
        // taskflow, and with it this very node. Touching `target` afterwards is
        // a use-after-free that only shows up when the waiter is quick.
        //
        // TSan found this; 61 passing tests did not.
        auto *async_counter = target->async_counter;

        if(run != nullptr) {
            finish_node(*run);
        }

        // An async node has no graph and no successors: it belongs to whoever
        // spawned it, and this is where its life ends. Safe to touch here
        // precisely because an async node has no topology to complete -- so
        // nobody was released above.
        if(async_counter != nullptr) {
            delete target;
            // Release-ordered, so a corun() that observes zero also observes
            // everything the task did.
            async_counter->fetch_sub(1u, stl::memory_order_acq_rel);
        }

        if(cache != nullptr) {
            target = cache;
            cache = nullptr;
            worker::bump(self.continuations);
            goto begin_invoke;
        }
    }

    /**
     * Decrement each ready successor's join counter; whoever drives one to zero
     * schedules it. Returns one successor to run directly, if there is exactly
     * one -- that is the continuation.
     */
    [[nodiscard]] node *release_successors(node *target, const int chosen) {
        node *continuation = nullptr;

        if(target->handle.index() == node::is_condition) {
            // A condition schedules exactly one successor, and does NOT touch
            // the others' join counters -- they are simply not taken this pass.
            if(chosen >= 0 && static_cast<stl::size_t>(chosen) < target->num_successors()) {
                auto *next = target->successor(static_cast<stl::size_t>(chosen));
                next->nstate_word |= nstate::conditioned;

                if(target->run != nullptr) {
                    target->run->pending.fetch_add(1u, stl::memory_order_acq_rel);
                }

                return next;
            }

            return nullptr;
        }

        for(stl::size_t pos = 0u; pos < target->num_successors(); ++pos) {
            auto *next = target->successor(pos);

            // A conditioned successor was already released by its condition.
            if((next->nstate_word & nstate::conditioned) != 0u) {
                continue;
            }

            if(next->join_counter.fetch_sub(1u, stl::memory_order_acq_rel) == 1u) {
                if(target->run != nullptr) {
                    target->run->pending.fetch_add(1u, stl::memory_order_acq_rel);
                }

                if(continuation == nullptr) {
                    continuation = next; // keep the first one for ourselves
                } else {
                    schedule(next);
                }
            }
        }

        return continuation;
    }

    void capture(topology *run, node *target) {
        target->mark(estate::exception);

        if(run == nullptr) {
            return;
        }

        run->exceptions.fetch_add(1u, stl::memory_order_acq_rel);

        {
            const std::lock_guard guard{run->mutex};

            // FIRST WINS. Several tasks can throw simultaneously and there is no
            // std::exception_ptr that means "these three"; picking the first and
            // counting the rest is a decision, documented in NOTES.md, not an
            // accident.
            if(!run->captured) {
                run->captured = std::current_exception();
            }
        }

        // Cancel the rest of the graph: remaining tasks are skipped, but their
        // join counters still resolve so the run can finish and be waited on.
        run->cancel.store(true, stl::memory_order_release);
    }

    void finish_node(topology &run) {
        if(run.pending.fetch_sub(1u, stl::memory_order_acq_rel) == 1u) {
            complete(run);
        }
    }

    static void complete(topology &run) {
        // notify_all INSIDE the lock, which looks like the pessimisation
        // everyone is taught to avoid and is here a correctness requirement.
        //
        // With the notify outside, the waiter can return from wait(), destroy
        // the topology and its condition_variable while this thread is still
        // inside notify_all -- a use-after-free that TSan caught and that 61
        // passing tests did not. Holding the lock across the notify means the
        // waiter cannot leave wait() (it must reacquire the mutex first) until
        // notify_all has returned and the guard has released.
        //
        // The usual advice to notify outside the lock assumes the condition
        // variable outlives both parties. Here the waiter owns it.
        const std::lock_guard guard{run.mutex};
        run.finished = true;
        run.cv.notify_all();
    }

    stl::vector<worker> workers;
    stl::vector<std::thread> threads;

    // Overflow for work that did not fit a bounded queue, and for submissions
    // from outside the pool. Locked, because it is off the hot path by
    // construction: the fast path is a worker pushing to its own deque.
    std::mutex overflow_mutex;
    unbounded_wsq<node *> overflow;

    nonblocking_notifier notifier;
    stl::atomic<bool> done{false};
    bool sticky_enabled{true};

    static inline thread_local worker *current_worker = nullptr;
};

template<typename Fn>
void runtime::silent_async(Fn &&work) {
    host->spawn_async(stl::function<void()>{stl::forward<Fn>(work)}, spawned);
}

inline void runtime::corun() {
    host->corun_until([this] { return spawned.load(stl::memory_order_acquire) == 0u; });
}

template<typename Predicate>
void runtime::corun_until(Predicate &&stop) {
    host->corun_until(stl::forward<Predicate>(stop));
}

} // namespace acpp

#endif // ACPP_EXECUTOR_HPP
