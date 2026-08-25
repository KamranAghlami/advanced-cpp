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
// A line waiting for its ticket backs off (yield, then a short bounded sleep)
// rather than calling corun -- and that distinction is the sharpest thing in
// this file. See "why not corun here" below.

#include "algorithm.hpp"
#include "config.hpp"
#include "executor.hpp"
#include "stl/atomic.hpp"
#include "stl/cstddef.hpp"
#include "stl/functional.hpp"
#include "stl/utility.hpp"
#include "stl/vector.hpp"

#include <chrono>
#include <thread>

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
 *
 * Each line occupies a worker for its whole life, so `lines` above the pool's
 * worker count buys nothing and costs a worker that could have been stealing.
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

        // The stop point is a TOKEN, not a flag, and that distinction is a bug
        // this file already had. With a bool, a line holding token 199 that was
        // still queued for the *sink* when the source stopped at token 200
        // would read `stopped == true` and skip its own sink stage -- dropping
        // a token that was legitimately in flight. Only reproduces with real
        // parallelism; a single core serialises the lines enough to hide it.
        //
        // With a token, the test is `token >= stop_token`, which is false for
        // everything already in flight and true for everything after.
        stl::atomic<stl::size_t> stop_token{no_stop};

        for(stl::size_t line = 0u; line < line_count; ++line) {
            rt.silent_async([this, &next_token, &stop_token, line] {
                drive(next_token, stop_token, line);
            });
        }

        rt.corun();
    }

private:
    void drive(stl::atomic<stl::size_t> &next_token, stl::atomic<stl::size_t> &stop_token,
               const stl::size_t line) {
        for(;;) {
            const auto token = next_token.fetch_add(1u, stl::memory_order_relaxed);

            // The invariant that keeps this from deadlocking: EVERY claimed
            // token walks EVERY stage and advances every serial ticket, even
            // when there is no longer any work to do. A token that returned
            // early would leave the stages after it waiting for a ticket value
            // that nobody would ever publish.
            bool skip = token >= stop_token.load(stl::memory_order_acquire);

            pipeflow flow;
            flow.current_token = token;
            flow.current_line = line;

            for(stl::size_t stage = 0u; stage < stages.size(); ++stage) {
                flow.current_pipe = stage;

                if(stages[stage].type == pipe_type::serial) {
                    // ---------------------------------------------------------
                    // WHY NOT corun HERE.
                    //
                    // corun is the right answer for waiting on work you
                    // SPAWNED: it can only ever run your own subtree, so
                    // helping cannot deepen the thing you are waiting for.
                    //
                    // A pipeline line waits on a PEER -- the line holding the
                    // previous token. corun would pop that peer onto this
                    // thread's stack *below* us, and a peer holding a lower
                    // token is exactly the one that must finish before we can
                    // proceed. Nest four lines on one stack and the innermost
                    // waits on the outermost, which is blocked waiting on the
                    // innermost. Deadlock.
                    //
                    // Found the hard way: the ordering test did not finish in
                    // 900 s under TSan, where one core makes the nesting
                    // certain instead of merely possible.
                    //
                    // Backing off instead leaves the peer in a queue where
                    // another worker can steal it, which is what makes progress.
                    // ---------------------------------------------------------
                    wait_for_ticket(stage, token);

                    // Re-read after waiting: the stream may have stopped while
                    // this line was queued. Compared by TOKEN, so a token that
                    // was already in flight when the stop happened is not
                    // dropped part-way through the pipeline.
                    skip = skip || (token >= stop_token.load(stl::memory_order_acquire));
                }

                if(!skip) {
                    stages[stage].callable(flow);

                    if(flow.stopped) {
                        // The source says this is the end. This token stops
                        // here, but it still has to walk the remaining stages
                        // to advance their tickets.
                        //
                        // min, not store: two lines could both stop (a second
                        // source call can only happen at a higher token, but
                        // being explicit costs nothing and the invariant
                        // "stop_token only decreases" is what makes the
                        // comparison above monotone).
                        auto current = stop_token.load(stl::memory_order_relaxed);
                        while(token < current
                              && !stop_token.compare_exchange_weak(current, token,
                                                                   stl::memory_order_release,
                                                                   stl::memory_order_relaxed)) {}
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

    /**
     * Yield, then sleep briefly. Bounded, so this is a wait and not a park:
     * progress never depends on anyone waking us, only on another worker
     * getting round to the peer line that owns the next ticket.
     */
    void wait_for_ticket(const stl::size_t stage, const stl::size_t token) const {
        for(unsigned idle = 0u; tickets[stage].load(stl::memory_order_acquire) != token; ++idle) {
            if(idle < 64u) {
                std::this_thread::yield();
            } else {
                std::this_thread::sleep_for(std::chrono::microseconds{50});
            }
        }
    }

    static constexpr stl::size_t no_stop = static_cast<stl::size_t>(-1);

    stl::vector<pipe> stages;
    stl::vector<stl::atomic<stl::size_t>> tickets;
    stl::size_t line_count;
};

} // namespace acpp

#endif // ACPP_PIPELINE_HPP
