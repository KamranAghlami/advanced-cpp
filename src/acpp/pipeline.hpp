#ifndef ACPP_PIPELINE_HPP
#define ACPP_PIPELINE_HPP

// Module 12 -- pipeline scheduling.
//
// Tokens flow through a fixed sequence of stages. Each stage is SERIAL (tokens
// must pass through it in order, one at a time) or PARALLEL (any number of
// tokens at once, in any order).
//
// The interesting constraint is the serial one, and the interesting question is
// how to enforce it WITHOUT A LOCK. The answer here, as in Taskflow, is a
// per-stage atomic ticket: a serial stage admits token `t` only when its
// counter reads `t`, and publishes `t + 1` on the way out. That is a
// release/acquire handoff between two tokens, not mutual exclusion -- there is
// never a moment where a thread holds something another thread must wait to
// acquire.
//
// A line that has to wait does NOT block: it calls corun, so the worker spends
// the wait executing other pipeline work. Blocking here would deadlock the pool
// at exactly the width where the pipeline gets interesting.

#include "algorithm.hpp"
#include "config.hpp"
#include "executor.hpp"
#include "stl/atomic.hpp"
#include "stl/cstddef.hpp"
#include "stl/functional.hpp"
#include "stl/utility.hpp"
#include "stl/vector.hpp"

namespace acpp {

enum class pipe_type : int {
    serial,   //< tokens pass through in order, one at a time
    parallel, //< any number of tokens, any order
};

/** What a stage callable is handed. */
class pipeflow {
    friend class pipeline;

public:
    [[nodiscard]] stl::size_t token() const noexcept { return current_token; }
    [[nodiscard]] stl::size_t line() const noexcept { return current_line; }
    [[nodiscard]] stl::size_t pipe() const noexcept { return current_pipe; }

    /** Called from the FIRST stage to end the stream. */
    void stop() noexcept { stopped = true; }

private:
    stl::size_t current_token{0u};
    stl::size_t current_line{0u};
    stl::size_t current_pipe{0u};
    bool stopped{false};
};

struct pipe {
    pipe_type type{pipe_type::serial};
    stl::function<void(pipeflow &)> callable{};
};

/**
 * A fixed pipeline over N lines.
 *
 * `lines` is how many tokens may be in flight at once -- the pipeline's depth.
 * More lines means more parallelism in the parallel stages and more memory for
 * in-flight state; it does not speed up the serial stages, which are the
 * bottleneck by construction.
 */
class pipeline {
public:
    pipeline(const stl::size_t lines, stl::vector<pipe> stages)
        : stages{stl::move(stages)}, line_count{lines == 0u ? 1u : lines} {
        tickets = stl::vector<stl::atomic<stl::size_t>>(this->stages.size());

        for(auto &ticket: tickets) {
            ticket.store(0u, stl::memory_order_relaxed);
        }
    }

    [[nodiscard]] stl::size_t num_lines() const noexcept { return line_count; }
    [[nodiscard]] stl::size_t num_pipes() const noexcept { return stages.size(); }

    /** Drive the pipeline to completion. Call from inside a runtime task. */
    void run(runtime &rt) {
        stl::atomic<stl::size_t> next_token{0u};
        stl::atomic<bool> stopped{false};

        for(stl::size_t line = 0u; line < line_count; ++line) {
            rt.silent_async([this, &rt, &next_token, &stopped, line] {
                drive(rt, next_token, stopped, line);
            });
        }

        rt.corun();
    }

private:
    void drive(runtime &rt, stl::atomic<stl::size_t> &next_token, stl::atomic<bool> &stopped,
               const stl::size_t line) {
        for(;;) {
            const auto token = next_token.fetch_add(1u, stl::memory_order_relaxed);

            // The invariant that keeps this from deadlocking: EVERY claimed
            // token walks EVERY stage and advances every serial ticket, even
            // when there is no longer any work to do. A token that returned
            // early would leave the stages after it waiting for a ticket value
            // that nobody would ever publish.
            bool skip = stopped.load(stl::memory_order_acquire);

            pipeflow flow;
            flow.current_token = token;
            flow.current_line = line;

            for(stl::size_t stage = 0u; stage < stages.size(); ++stage) {
                flow.current_pipe = stage;

                if(stages[stage].type == pipe_type::serial) {
                    // Wait for our turn WITHOUT blocking the worker: corun
                    // executes other pipeline lines while we wait. Blocking
                    // here deadlocks the pool at exactly the width where a
                    // pipeline gets interesting.
                    rt.corun_until([&] {
                        return tickets[stage].load(stl::memory_order_acquire) == token;
                    });

                    // Re-read after waiting: the stream may have stopped while
                    // this line was queued behind an earlier token.
                    skip = skip || stopped.load(stl::memory_order_acquire);
                }

                if(!skip) {
                    stages[stage].callable(flow);

                    if(flow.stopped) {
                        // The source says this is the end. This token stops
                        // here, but it still has to walk the remaining stages
                        // to advance their tickets.
                        stopped.store(true, stl::memory_order_release);
                        skip = true;
                    }
                }

                if(stages[stage].type == pipe_type::serial) {
                    // RELEASE, so everything this stage did is visible to
                    // whichever line acquires the next ticket value.
                    tickets[stage].store(token + 1u, stl::memory_order_release);
                }
            }

            if(skip) {
                return;
            }
        }
    }

    stl::vector<pipe> stages;
    stl::vector<stl::atomic<stl::size_t>> tickets;
    stl::size_t line_count;
};

} // namespace acpp

#endif // ACPP_PIPELINE_HPP
