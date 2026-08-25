#ifndef ACPP_NOTIFIER_HPP
#define ACPP_NOTIFIER_HPP

// Module 10 -- sleeping without lost wakeups.
//
// The problem, in three lines:
//
//   worker: checks every queue, finds nothing, decides to sleep
//   pusher: pushes work, signals -- nobody is registered yet, so the signal
//           goes nowhere
//   worker: parks, and sleeps forever with work pending
//
// Spinning instead is correct and burns power. A mutex + condvar on every push
// is correct and puts a lock on the hot path. What you want is: no
// synchronisation on push when nobody is sleeping, and a correct wakeup when
// somebody is.
//
// The answer is a two-phase commit:
//
//   prepare_wait(id);          // announce the intent to sleep
//   ... re-check the predicate ...
//   found ? cancel_wait(id) : commit_wait(id);
//
// THE RULE: prepare_wait must be followed by EXACTLY ONE of commit_wait or
// cancel_wait, with the same id. Violating it is undefined behaviour.
//
// It works because prepare_wait publishes "a thread is about to sleep" BEFORE
// the predicate is re-checked. Any notifier arriving after the check but before
// the park sees the pre-waiter and delivers a wakeup anyway. The window is
// closed by ordering, not by a lock.
//
// Two implementations with the same interface:
//
//   blocking_notifier      mutex + condvar internals, correct protocol.
//                          Get the protocol right before getting it lock-free.
//   nonblocking_notifier   one packed atomic word and an intrusive stack of
//                          parked waiters. Descends from Vyukov's EventCount.

#include "config.hpp"
#include "stl/atomic.hpp"
#include "stl/cstddef.hpp"
#include "stl/cstdint.hpp"
#include "stl/vector.hpp"

#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace acpp {

/**
 * The protocol, with the internals kept obvious.
 *
 * Everything is under one mutex, so the invariant can be stated and checked
 * rather than argued: a signal is either delivered to a committed waiter, or
 * held for a pre-waiter that is about to commit, or dropped because nobody is
 * waiting at all -- and the third case is safe because a thread that is not in
 * the protocol has not yet re-checked its predicate.
 */
class blocking_notifier {
    struct waiter {
        std::condition_variable cv;
        bool parked{false};
        bool signaled{false};
    };

public:
    explicit blocking_notifier(const stl::size_t count)
        : waiters(count) {}

    blocking_notifier(const blocking_notifier &) = delete;
    blocking_notifier &operator=(const blocking_notifier &) = delete;

    [[nodiscard]] stl::size_t size() const noexcept { return waiters.size(); }

    void prepare_wait(const stl::size_t) {
        const std::lock_guard guard{mutex};
        ++prewaiters;
    }

    void cancel_wait(const stl::size_t) {
        const std::lock_guard guard{mutex};
        --prewaiters;
        // We are leaving the protocol having found work. A signal that was held
        // for a pre-waiter must not evaporate with us -- redistribute it.
        redistribute();
    }

    void commit_wait(const stl::size_t id) {
        std::unique_lock guard{mutex};
        --prewaiters;

        // A notify arrived inside the window. Consume it and do not park: this
        // is the lost wakeup, caught.
        if(signals > 0u) {
            --signals;
            return;
        }

        auto &self = waiters[id];
        self.parked = true;
        self.signaled = false;
        ++parked_count;

        self.cv.wait(guard, [&self] { return self.signaled; });

        self.parked = false;
        self.signaled = false;
        --parked_count;
    }

    void notify_one() {
        const std::lock_guard guard{mutex};

        if(wake_one_parked()) {
            return;
        }

        // Nobody parked. If somebody is between prepare_wait and commit_wait,
        // hold the signal for them; otherwise there is nobody to tell, and a
        // thread not in the protocol has not yet re-checked its predicate.
        if(prewaiters > signals) {
            ++signals;
        }
    }

    void notify_all() {
        const std::lock_guard guard{mutex};

        while(wake_one_parked()) {}

        signals = prewaiters;
    }

    /** Diagnostics for the exercises; not part of the protocol. */
    [[nodiscard]] stl::size_t parked() const {
        const std::lock_guard guard{mutex};
        return parked_count;
    }

private:
    bool wake_one_parked() {
        for(auto &candidate: waiters) {
            if(candidate.parked && !candidate.signaled) {
                candidate.signaled = true;
                candidate.cv.notify_one();
                return true;
            }
        }

        return false;
    }

    void redistribute() {
        // Only meaningful once the pre-waiters a signal was held for are gone.
        while(signals > prewaiters && wake_one_parked()) {
            --signals;
        }

        if(prewaiters == 0u && parked_count == 0u) {
            signals = 0u;
        }
    }

    mutable std::mutex mutex;
    stl::vector<waiter> waiters;
    stl::size_t prewaiters{0u};
    stl::size_t parked_count{0u};
    stl::size_t signals{0u};
};

/**
 * The same protocol with one 64-bit word and an intrusive stack.
 *
 * Layout of `state`, low bits first:
 *
 *   [ 0, 16)  stack       index of the waiter at the top of the parked stack,
 *                         or STACK_MASK (all ones) for "empty"
 *   [16, 32)  prewaiters  how many threads are between prepare and commit
 *   [32, 64)  epoch       bumped on every fulfilled request
 *
 * The epoch is the ABA defence: every CAS that resolves a request bumps it, so
 * a stale read cannot succeed even when the stack index happens to match. It is
 * also a ticket: a pre-waiter records the state it saw, and `epoch - recorded`
 * tells it whether its turn has arrived, passed, or not yet come.
 */
class nonblocking_notifier {
    struct waiter {
        stl::atomic<waiter *> next{nullptr};
        std::mutex mutex;
        std::condition_variable cv;
        stl::uint64_t epoch{0u};
        unsigned state{not_signaled};
    };

    static constexpr unsigned not_signaled = 0u;
    static constexpr unsigned waiting = 1u;
    static constexpr unsigned signaled = 2u;

public:
    static constexpr stl::uint64_t stack_bits = 16u;
    static constexpr stl::uint64_t stack_mask = (stl::uint64_t{1} << stack_bits) - 1u;

    static constexpr stl::uint64_t prewaiter_bits = 16u;
    static constexpr stl::uint64_t prewaiter_shift = 16u;
    static constexpr stl::uint64_t prewaiter_mask = ((stl::uint64_t{1} << prewaiter_bits) - 1u) << prewaiter_shift;
    static constexpr stl::uint64_t prewaiter_inc = stl::uint64_t{1} << prewaiter_shift;

    static constexpr stl::uint64_t epoch_bits = 32u;
    static constexpr stl::uint64_t epoch_shift = 32u;
    static constexpr stl::uint64_t epoch_mask = ((stl::uint64_t{1} << epoch_bits) - 1u) << epoch_shift;
    static constexpr stl::uint64_t epoch_inc = stl::uint64_t{1} << epoch_shift;

    /** One index is spent on the "empty stack" sentinel. */
    static constexpr stl::size_t max_waiters = static_cast<stl::size_t>(stack_mask) - 1u;

    explicit nonblocking_notifier(const stl::size_t count)
        : state{stack_mask}, waiters(count) {
        // Not an assert: exceeding this silently aliases a waiter index with the
        // empty-stack sentinel, which is a hang, not a crash.
        if(count > max_waiters) {
            throw std::length_error{"nonblocking_notifier: too many waiters for the stack field"};
        }
    }

    nonblocking_notifier(const nonblocking_notifier &) = delete;
    nonblocking_notifier &operator=(const nonblocking_notifier &) = delete;

    [[nodiscard]] stl::size_t size() const noexcept { return waiters.size(); }

    void prepare_wait(const stl::size_t id) {
        // Record the state as of entering the protocol, and become a pre-waiter
        // in one RMW. The fence is what makes this *publish* before the caller
        // re-checks its predicate -- without it, the increment may be sitting in
        // a store buffer while the predicate load has already happened, and the
        // whole two-phase commit buys nothing.
        waiters[id].epoch = state.fetch_add(prewaiter_inc, stl::memory_order_relaxed);
        stl::atomic_thread_fence(stl::memory_order_seq_cst);
    }

    void cancel_wait(const stl::size_t id) {
        const auto ticket = ticket_of(waiters[id].epoch);
        auto current = state.load(stl::memory_order_relaxed);

        for(;;) {
            const auto position = static_cast<stl::int64_t>((current & epoch_mask) - ticket);

            if(position < 0) {
                // An earlier pre-waiter has not resolved yet. Requests are
                // handled in ticket order, so wait for it.
                std::this_thread::yield();
                current = state.load(stl::memory_order_relaxed);
                continue;
            }

            if(position > 0) {
                return; // already notified; the signal was ours
            }

            if(state.compare_exchange_weak(current, current - prewaiter_inc + epoch_inc,
                                           stl::memory_order_relaxed)) {
                return;
            }
        }
    }

    void commit_wait(const stl::size_t id) {
        auto *self = &waiters[id];
        self->state = not_signaled;

        const auto ticket = ticket_of(self->epoch);
        auto current = state.load(stl::memory_order_seq_cst);

        for(;;) {
            const auto position = static_cast<stl::int64_t>((current & epoch_mask) - ticket);

            if(position < 0) {
                std::this_thread::yield();
                current = state.load(stl::memory_order_seq_cst);
                continue;
            }

            if(position > 0) {
                return; // notified during the window -- do not park
            }

            // Leave the pre-wait count, bump the epoch, and push onto the stack.
            auto next = current - prewaiter_inc + epoch_inc;
            next = (next & ~stack_mask) | static_cast<stl::uint64_t>(id);

            self->next.store((current & stack_mask) == stack_mask
                                 ? nullptr
                                 : &waiters[static_cast<stl::size_t>(current & stack_mask)],
                             stl::memory_order_relaxed);

            // RELEASE: the `next` pointer above must be visible to whichever
            // thread later pops this waiter off the stack.
            if(state.compare_exchange_weak(current, next, stl::memory_order_release)) {
                break;
            }
        }

        park(self);
    }

    void notify_one() { notify(false); }
    void notify_all() { notify(true); }

private:
    [[nodiscard]] static stl::uint64_t ticket_of(const stl::uint64_t recorded) noexcept {
        // The epoch this waiter must see before its turn: the epoch it recorded,
        // plus its position in the pre-waiter queue.
        return (recorded & epoch_mask) + (((recorded & prewaiter_mask) >> prewaiter_shift) << epoch_shift);
    }

    void notify(const bool all) {
        // Orders the caller's push against this load, so a notifier that pushed
        // work cannot then read a state word from before the pusher was visible.
        stl::atomic_thread_fence(stl::memory_order_seq_cst);
        auto current = state.load(stl::memory_order_acquire);

        for(;;) {
            // The fast path the whole design exists for: nobody waiting, one
            // relaxed-ish load, no lock, no syscall.
            if((current & stack_mask) == stack_mask && (current & prewaiter_mask) == 0u) {
                return;
            }

            const auto prewaiters = (current & prewaiter_mask) >> prewaiter_shift;
            stl::uint64_t next = 0u;

            if(all) {
                next = (current & epoch_mask) + (epoch_inc * prewaiters) + stack_mask;
            } else if(prewaiters != 0u) {
                // Somebody is mid-protocol. Consuming its pre-wait slot IS the
                // wakeup: it will see the bumped epoch and decline to park.
                next = current + epoch_inc - prewaiter_inc;
            } else {
                auto *top = &waiters[static_cast<stl::size_t>(current & stack_mask)];
                auto *following = top->next.load(stl::memory_order_relaxed);
                const auto index = (following == nullptr)
                                       ? stack_mask
                                       : static_cast<stl::uint64_t>(following - waiters.data());
                // No epoch bump here, and that is deliberate: a waiter can only
                // be re-pushed after passing through the pre-wait state, which
                // always bumps the epoch. So ABA on the stack cannot happen.
                next = (current & epoch_mask) + index;
            }

            if(state.compare_exchange_weak(current, next, stl::memory_order_acquire)) {
                if(!all && prewaiters != 0u) {
                    return;
                }

                if(all) {
                    if((current & stack_mask) == stack_mask) {
                        return;
                    }

                    auto *top = &waiters[static_cast<stl::size_t>(current & stack_mask)];
                    unpark_list(top);
                    return;
                }

                auto *top = &waiters[static_cast<stl::size_t>(current & stack_mask)];
                top->next.store(nullptr, stl::memory_order_relaxed);
                unpark(top);
                return;
            }
        }
    }

    static void park(waiter *self) {
        std::unique_lock guard{self->mutex};
        self->state = waiting;
        self->cv.wait(guard, [self] { return self->state == signaled; });
    }

    static void unpark(waiter *target) {
        {
            const std::lock_guard guard{target->mutex};
            target->state = signaled;
        }

        target->cv.notify_one();
    }

    static void unpark_list(waiter *head) {
        while(head != nullptr) {
            auto *following = head->next.load(stl::memory_order_relaxed);
            head->next.store(nullptr, stl::memory_order_relaxed);
            unpark(head);
            head = following;
        }
    }

    alignas(ACPP_CACHELINE_SIZE) stl::atomic<stl::uint64_t> state;
    stl::vector<waiter> waiters;
};

} // namespace acpp

#endif // ACPP_NOTIFIER_HPP
