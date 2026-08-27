#ifndef ACPP_WSQ_HPP
#define ACPP_WSQ_HPP

// Module 9 -- the Chase-Lev work-stealing deque.
//
// Reference: Le, Pop, Cohen, Zappa Nardelli, "Correct and Efficient
// Work-Stealing for Weak Memory Models", PPoPP'13. Every memory order below is
// from that paper; modules/09-work-stealing-deque/NOTES.md defends each one.
//
// The contract:
//   * ONE owner thread calls push() and pop(), at the BOTTOM.
//   * MANY thief threads call steal(), at the TOP.
//
// The owner works LIFO, so it gets the most recently pushed task -- hot in this
// core's cache. Thieves work FIFO, so they take the oldest task -- typically the
// coarsest-grained one, and the one furthest from the owner's working set. That
// asymmetry is the whole design, not an accident of implementation.

#include "config.hpp"
#include "stl/array.hpp"
#include "stl/atomic.hpp"
#include "stl/cstddef.hpp"
#include "stl/cstdint.hpp"
#include "stl/optional.hpp"
#include "stl/type_traits.hpp"
#include "stl/utility.hpp"
#include "stl/vector.hpp"

// ---------------------------------------------------------------------------
// An experiment, not a knob.
//
// Module 9's exercise 3 asks you to weaken one memory order deliberately and see
// whether your test catches it. Defining ACPP_WSQ_WEAKEN_FENCE turns the two
// seq_cst fences into acq_rel, which is exactly the wrong thing: acq_rel on a
// fence does not order a prior STORE against a subsequent LOAD, and that is the
// only ordering these fences exist to provide.
//
// It is compiled only by the wsq_weakened target. Never define it anywhere else.
//
// ACPP_WSQ_WEAKEN_RELEASE is the second knob, and it exists because the first one
// turned out to be untestable on ARM. Measured (Apple clang 21, arm64):
//
//                          AArch64      x86-64
//   seq_cst fence          dmb ish      lock orl $0, -64(%rsp)
//   acq_rel fence          dmb ish      <nothing>
//   release store          stlr         movq
//   relaxed store          str          movq
//
// So WEAKEN_FENCE changes no instruction on ARM, and WEAKEN_RELEASE changes no
// instruction on x86. The two machines catch complementary halves of the memory
// model, and neither one alone is a test of both. See NOTES.md, exercise 3.
//
// x86-64 side confirmed on real hardware 2026-08-27: wsq_weakened fails 199/200
// on 16 real cores under clang (which does drop the acq_rel barrier, per the
// table above) and 0/50 under gcc (which does not -- gcc emits the seq_cst
// barrier unconditionally on x86-64, a second vacuous pairing next to clang/ARM
// above). wsq_weakened is a live CI-flakiness risk under clang with real
// parallelism; see NOTES.md and docs/pending-verification.md.
//
// What WEAKEN_RELEASE breaks: `push` writes the slot with a relaxed store and
// then publishes it by storing the new `bottom`. The release on that store is
// what makes the slot write visible to a thief that acquires `bottom` in
// `steal`. Demoted to relaxed, the two stores may become visible out of order,
// and a thief can read a slot the owner has not written yet.
// ---------------------------------------------------------------------------
#if defined ACPP_WSQ_WEAKEN_FENCE
#    define ACPP_WSQ_FENCE_ORDER ::acpp::stl::memory_order_acq_rel
#else
#    define ACPP_WSQ_FENCE_ORDER ::acpp::stl::memory_order_seq_cst
#endif

#if defined ACPP_WSQ_WEAKEN_RELEASE
#    define ACPP_WSQ_PUBLISH_ORDER ::acpp::stl::memory_order_relaxed
#else
#    define ACPP_WSQ_PUBLISH_ORDER ::acpp::stl::memory_order_release
#endif

namespace acpp {

/**
 * The "nothing to take" sentinel.
 *
 * Encoded in the return value rather than an out-parameter or a status enum:
 * pointer element types use nullptr, everything else uses std::optional. A
 * steal that loses its CAS returns the same thing as a steal from an empty
 * queue, because from the caller's side those are the same situation.
 */
template<typename Type>
[[nodiscard]] constexpr auto wsq_empty_value() noexcept {
    if constexpr(stl::is_pointer_v<Type>) {
        return Type{nullptr};
    } else {
        return stl::optional<Type>{stl::nullopt};
    }
}

template<typename Type>
using wsq_value_t = decltype(wsq_empty_value<Type>());

/**
 * Fixed-capacity Chase-Lev deque.
 *
 * Capacity is a power of two so wrapping is a mask. `top` and `bottom` are
 * free-running int64 counters, never wrapped: 2^63 pushes is not a number any
 * program reaches, and free-running counters are what make "empty" and "full"
 * decidable without a separate count and what removes ABA on `top` entirely.
 */
template<typename Type, stl::size_t LogSize = 8u>
class bounded_wsq {
public:
    using value_type = wsq_value_t<Type>;

    static constexpr stl::int64_t capacity = stl::int64_t{1} << LogSize;
    static constexpr stl::int64_t mask = capacity - 1;

    [[nodiscard]] static constexpr value_type empty_value() noexcept { return wsq_empty_value<Type>(); }

    bounded_wsq() noexcept
        : top{0}, bottom{0} {}

    bounded_wsq(const bounded_wsq &) = delete;
    bounded_wsq &operator=(const bounded_wsq &) = delete;

    /** Owner only. Returns false if the queue is full. */
    bool try_push(Type item) noexcept {
        const auto b = bottom.load(stl::memory_order_relaxed);
        const auto t = top.load(stl::memory_order_acquire);

        if(b - t >= capacity) {
            return false;
        }

        buffer[static_cast<stl::size_t>(b & mask)].store(item, stl::memory_order_relaxed);

        // RELEASE, and this is the one everybody gets right: it publishes the
        // slot write to any thief that later acquires `bottom`.
        bottom.store(b + 1, ACPP_WSQ_PUBLISH_ORDER);
        return true;
    }

    /**
     * Owner only. Takes from the bottom, LIFO.
     *
     * The four questions this function exists to make you answer are in NOTES.md.
     */
    value_type pop() noexcept {
        // Claim the slot first, ask questions later. Publishing the decrement
        // before reading `top` is what makes the race with a thief resolvable
        // at all -- a thief that reads this new bottom will not touch the slot.
        const auto b = bottom.load(stl::memory_order_relaxed) - 1;
        bottom.store(b, stl::memory_order_relaxed);

        // THE fence. It stops the store to `bottom` above from being reordered
        // after the load of `top` below. Without it the owner and a thief can
        // both conclude they took the last element. See NOTES.md, question 2.
        stl::atomic_thread_fence(ACPP_WSQ_FENCE_ORDER);

        auto t = top.load(stl::memory_order_relaxed);
        auto item = empty_value();

        if(t <= b) {
            item = read_at(b);

            if(t == b) {
                // Exactly one element, and a thief may be reaching for it. Both
                // sides settle it by CAS-ing `top`; exactly one wins.
                if(!top.compare_exchange_strong(t, t + 1, stl::memory_order_seq_cst,
                                                stl::memory_order_relaxed)) {
                    item = empty_value();
                }

                bottom.store(b + 1, stl::memory_order_relaxed);
            }
        } else {
            // Empty. Undo the speculative decrement.
            bottom.store(b + 1, stl::memory_order_relaxed);
        }

        return item;
    }

    /** Any thief. Takes from the top, FIFO. */
    value_type steal() noexcept {
        auto t = top.load(stl::memory_order_acquire);

        // Mirror of the owner's fence: it orders this thief's read of `top`
        // before its read of `bottom`, which is what makes the two sides agree
        // about which of them is looking at the last element.
        stl::atomic_thread_fence(ACPP_WSQ_FENCE_ORDER);

        const auto b = bottom.load(stl::memory_order_acquire);

        if(t < b) {
            // The item is read BEFORE the CAS succeeds, so this may read a slot
            // the owner is concurrently overwriting. Benign only because the CAS
            // then fails and the value is discarded -- see NOTES.md.
            auto item = read_at(t);

            if(!top.compare_exchange_strong(t, t + 1, stl::memory_order_seq_cst,
                                            stl::memory_order_relaxed)) {
                return empty_value();
            }

            return item;
        }

        return empty_value();
    }

    /** A snapshot, immediately stale for anyone but the owner. */
    [[nodiscard]] stl::int64_t size() const noexcept {
        const auto b = bottom.load(stl::memory_order_relaxed);
        const auto t = top.load(stl::memory_order_relaxed);
        return b >= t ? b - t : 0;
    }

    [[nodiscard]] bool empty() const noexcept { return size() == 0; }

private:
    [[nodiscard]] value_type read_at(const stl::int64_t index) const noexcept {
        const auto raw = buffer[static_cast<stl::size_t>(index & mask)].load(stl::memory_order_relaxed);

        if constexpr(stl::is_pointer_v<Type>) {
            return raw;
        } else {
            return stl::optional<Type>{raw};
        }
    }

    // Three separate cache lines. The owner hammers `bottom`, thieves hammer
    // `top`, and putting them together would make every steal attempt invalidate
    // the owner's line. The buffer is separated for the same reason.
    alignas(ACPP_CACHELINE_SIZE) stl::atomic<stl::int64_t> top;
    alignas(ACPP_CACHELINE_SIZE) stl::atomic<stl::int64_t> bottom;
    alignas(ACPP_CACHELINE_SIZE) stl::array<stl::atomic<Type>, static_cast<stl::size_t>(capacity)> buffer{};
};

/**
 * Growing Chase-Lev deque.
 *
 * Same algorithm; the buffer can be replaced. The replaced buffer cannot be
 * freed -- a thief may still hold a pointer to it -- so old buffers are retained
 * until destruction. That is a deliberate memory-for-complexity trade: the
 * alternative is hazard pointers or epoch reclamation, and it is acceptable here
 * only because resizes are rare and each doubles the capacity, so the retained
 * garbage is bounded by the final size.
 */
template<typename Type>
class unbounded_wsq {
    struct buffer_type {
        stl::int64_t capacity;
        stl::int64_t mask;
        stl::atomic<Type> *slots;

        explicit buffer_type(const stl::int64_t cap)
            : capacity{cap}, mask{cap - 1}, slots{new stl::atomic<Type>[static_cast<stl::size_t>(cap)]} {}

        buffer_type(const buffer_type &) = delete;
        buffer_type &operator=(const buffer_type &) = delete;

        ~buffer_type() { delete[] slots; }

        void store(const stl::int64_t index, Type item) noexcept {
            slots[static_cast<stl::size_t>(index & mask)].store(item, stl::memory_order_relaxed);
        }

        [[nodiscard]] Type load(const stl::int64_t index) const noexcept {
            return slots[static_cast<stl::size_t>(index & mask)].load(stl::memory_order_relaxed);
        }

        [[nodiscard]] buffer_type *grow(const stl::int64_t b, const stl::int64_t t) const {
            auto *bigger = new buffer_type{capacity * 2};

            for(stl::int64_t pos = t; pos != b; ++pos) {
                bigger->store(pos, load(pos));
            }

            return bigger;
        }
    };

public:
    using value_type = wsq_value_t<Type>;

    [[nodiscard]] static constexpr value_type empty_value() noexcept { return wsq_empty_value<Type>(); }

    explicit unbounded_wsq(const stl::size_t log_size = 10u)
        : top{0}, bottom{0}, buffer{new buffer_type{stl::int64_t{1} << log_size}} {}

    unbounded_wsq(const unbounded_wsq &) = delete;
    unbounded_wsq &operator=(const unbounded_wsq &) = delete;

    ~unbounded_wsq() {
        for(auto *old: garbage) {
            delete old;
        }

        delete buffer.load(stl::memory_order_relaxed);
    }

    /** Owner only. Grows rather than failing. */
    void push(Type item) {
        const auto b = bottom.load(stl::memory_order_relaxed);
        auto *array = buffer.load(stl::memory_order_relaxed);

        // `cached_top` is a monotonic LOWER bound on occupancy: `top` only ever
        // increases, so a stale cached value can only *overestimate* how full
        // the queue is. Overestimating is safe -- it costs an unnecessary reload
        // of the real `top`, never a missed resize. That is the whole
        // correctness argument, and it is why the fast path can skip the load.
        if(array->capacity < (b - cached_top + 1)) [[unlikely]] {
            cached_top = top.load(stl::memory_order_acquire);

            if(array->capacity < (b - cached_top + 1)) [[unlikely]] {
                array = replace(array, b, cached_top);
            }
        }

        array->store(b, item);
        bottom.store(b + 1, ACPP_WSQ_PUBLISH_ORDER);
    }

    value_type pop() noexcept {
        const auto b = bottom.load(stl::memory_order_relaxed) - 1;
        auto *array = buffer.load(stl::memory_order_relaxed);
        bottom.store(b, stl::memory_order_relaxed);
        stl::atomic_thread_fence(ACPP_WSQ_FENCE_ORDER);

        auto t = top.load(stl::memory_order_relaxed);
        auto item = empty_value();

        if(t <= b) {
            item = wrap(array->load(b));

            if(t == b) {
                if(!top.compare_exchange_strong(t, t + 1, stl::memory_order_seq_cst,
                                                stl::memory_order_relaxed)) {
                    item = empty_value();
                }

                bottom.store(b + 1, stl::memory_order_relaxed);
            }
        } else {
            bottom.store(b + 1, stl::memory_order_relaxed);
        }

        return item;
    }

    value_type steal() noexcept {
        auto t = top.load(stl::memory_order_acquire);
        stl::atomic_thread_fence(ACPP_WSQ_FENCE_ORDER);
        const auto b = bottom.load(stl::memory_order_acquire);

        if(t < b) {
            // ACQUIRE, not relaxed: the buffer pointer must be read after
            // `bottom`, and the slots it points at must be visible. The paper
            // says consume; nobody implements consume, so acquire it is -- and
            // Taskflow's source carries the same note about TSan complaining.
            auto *array = buffer.load(stl::memory_order_acquire);
            auto item = wrap(array->load(t));

            if(!top.compare_exchange_strong(t, t + 1, stl::memory_order_seq_cst,
                                            stl::memory_order_relaxed)) {
                return empty_value();
            }

            return item;
        }

        return empty_value();
    }

    [[nodiscard]] stl::int64_t size() const noexcept {
        const auto b = bottom.load(stl::memory_order_relaxed);
        const auto t = top.load(stl::memory_order_relaxed);
        return b >= t ? b - t : 0;
    }

    [[nodiscard]] bool empty() const noexcept { return size() == 0; }

    [[nodiscard]] stl::int64_t capacity() const noexcept {
        return buffer.load(stl::memory_order_relaxed)->capacity;
    }

    /** Owner-only instrumentation; exercise 4 asks how often this really fires. */
    [[nodiscard]] stl::size_t resizes() const noexcept { return resize_count; }
    [[nodiscard]] stl::size_t retained_buffers() const noexcept { return garbage.size(); }

private:
    [[nodiscard]] static value_type wrap(Type raw) noexcept {
        if constexpr(stl::is_pointer_v<Type>) {
            return raw;
        } else {
            return stl::optional<Type>{raw};
        }
    }

    buffer_type *replace(buffer_type *array, const stl::int64_t b, const stl::int64_t t) {
        auto *bigger = array->grow(b, t);
        garbage.push_back(array);
        ++resize_count;

        // RELEASE: the copied slots must be visible before the new pointer is.
        // The paper allows relaxed here; release costs nothing on x86 and is
        // what lets TSan model the handoff.
        buffer.store(bigger, stl::memory_order_release);
        return bigger;
    }

    alignas(ACPP_CACHELINE_SIZE) stl::atomic<stl::int64_t> top;
    alignas(ACPP_CACHELINE_SIZE) stl::atomic<stl::int64_t> bottom;

    // Owner-private. Never read by a thief, so it does not need to be atomic and
    // does not belong on a contended line.
    stl::int64_t cached_top{0};
    stl::size_t resize_count{0};

    alignas(ACPP_CACHELINE_SIZE) stl::atomic<buffer_type *> buffer;
    stl::vector<buffer_type *> garbage;
};

} // namespace acpp

#endif // ACPP_WSQ_HPP
