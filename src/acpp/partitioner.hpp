#ifndef ACPP_PARTITIONER_HPP
#define ACPP_PARTITIONER_HPP

// Module 12 -- partitioners as policy objects.
//
// Get the structure right first, because it is easy to misread: there are FOUR
// partitioner classes and the type enum has only TWO values. Guided and random
// both report `dynamic`.
//
// That asymmetry is the design lesson of the file:
//
//   the ENUM encodes the SCHEDULING CONTRACT -- the only thing the algorithm
//   skeleton (for_each, reduce) needs to branch on. `static` means ranges are
//   pre-assigned per worker with no shared state; `dynamic` means workers pull
//   chunks from shared coordination.
//
//   the CLASS encodes the CHUNK-SIZING STRATEGY -- uniform, guided shrink,
//   random. It lives entirely inside loop() and never leaks into the algorithm.
//
// Two customization axes, deliberately kept at different visibility levels.

#include "config.hpp"
#include "stl/algorithm.hpp"
#include "stl/atomic.hpp"
#include "stl/cstddef.hpp"
#include "stl/type_traits.hpp"
#include "stl/utility.hpp"

#include <random>

namespace acpp {

/** The scheduling contract. Two values, four strategies. */
enum class partitioner_type : int {
    is_static,  //< ranges pre-assigned per worker; no shared cursor
    is_dynamic, //< workers pull chunks from a shared atomic cursor
};

/**
 * The default closure: call the work, wrap nothing.
 *
 * A tag type rather than `Closure = void`, because a member declared with a
 * parameter of type void is ill-formed at *class* instantiation -- a `requires`
 * clause on the constructor cannot rescue it. Empty, so
 * `[[no_unique_address]]` makes the default case free (Module 3).
 */
struct no_closure {
    template<typename Fn>
    constexpr decltype(auto) operator()(Fn &&work) const {
        return stl::forward<Fn>(work)();
    }
};

/**
 * The base every partitioner derives from.
 *
 * `Closure` is the injection point: a user can wrap every chunk in their own
 * setup/teardown -- a thread-local scratch buffer, NUMA pinning, a profiling
 * scope -- without the algorithm knowing anything about it.
 */
template<typename Closure = no_closure>
class partitioner_base {
public:
    constexpr partitioner_base() = default;

    explicit constexpr partitioner_base(const stl::size_t chunk) noexcept
        : chunk_size{chunk} {}

    constexpr partitioner_base(const stl::size_t chunk, Closure wrapper)
        : chunk_size{chunk}, closure{stl::move(wrapper)} {}

    [[nodiscard]] constexpr stl::size_t size() const noexcept { return chunk_size; }

    /** Wrap one chunk's work. With no_closure this is a direct call. */
    template<typename Fn>
    decltype(auto) wrap(Fn &&work) const {
        return closure(stl::forward<Fn>(work));
    }

protected:
    stl::size_t chunk_size{0u};

    [[no_unique_address]] Closure closure{};
};

/**
 * Even split, no shared state.
 *
 * `chunk_size == 0` means "auto": divide evenly and hand the remainder out one
 * item at a time to the first `N % W` workers, so no worker is more than one
 * item ahead of another.
 */
template<typename Closure = no_closure>
class static_partitioner: public partitioner_base<Closure> {
    using base_type = partitioner_base<Closure>;

public:
    using base_type::base_type;

    static constexpr partitioner_type type() noexcept { return partitioner_type::is_static; }

    [[nodiscard]] constexpr stl::size_t adjusted_chunk_size(const stl::size_t total,
                                                            const stl::size_t workers,
                                                            const stl::size_t worker) const noexcept {
        return this->chunk_size != 0u ? this->chunk_size
                                      : (total / workers + static_cast<stl::size_t>(worker < (total % workers)));
    }

    /**
     * Each worker strides by `workers * chunk`, so it gets an interleaved set of
     * chunks rather than one contiguous block. Interleaving is what keeps a
     * static split from being catastrophic when the work is not uniform across
     * the index range.
     */
    template<typename Fn>
    void loop(const stl::size_t total, const stl::size_t workers, const stl::size_t worker, Fn &&work) const {
        const auto chunk = adjusted_chunk_size(total, workers, worker);

        if(chunk == 0u) {
            return;
        }

        const auto stride = workers * chunk;

        for(auto begin = worker * chunk; begin < total; begin += stride) {
            const auto end = stl::min(begin + chunk, total);
            this->wrap([&] { work(begin, end); });
        }
    }
};

/** Uniform chunks pulled from a shared cursor. */
template<typename Closure = no_closure>
class dynamic_partitioner: public partitioner_base<Closure> {
    using base_type = partitioner_base<Closure>;

public:
    using base_type::base_type;

    static constexpr partitioner_type type() noexcept { return partitioner_type::is_dynamic; }

    template<typename Fn>
    void loop(const stl::size_t total, const stl::size_t, stl::atomic<stl::size_t> &cursor, Fn &&work) const {
        const auto chunk = this->chunk_size != 0u ? this->chunk_size : stl::size_t{1};

        for(;;) {
            const auto begin = cursor.fetch_add(chunk, stl::memory_order_relaxed);

            if(begin >= total) {
                return;
            }

            const auto end = stl::min(begin + chunk, total);
            this->wrap([&] { work(begin, end); });
        }
    }
};

/**
 * Large chunks first, shrinking toward the end.
 *
 * Large chunks amortise the cost of touching the shared cursor; small chunks at
 * the end give fine-grained load balancing on the tail, where one slow item
 * would otherwise make everyone wait.
 *
 * The shrink factor is `0.5 / workers` of what remains, which halves the
 * expected imbalance at each step, floored at `chunk_size`. Below
 * `2 * W * (chunk + 1)` remaining it switches to plain fetch_add chunks --
 * below that point a CAS loop costs more than it saves.
 */
template<typename Closure = no_closure>
class guided_partitioner: public partitioner_base<Closure> {
    using base_type = partitioner_base<Closure>;

public:
    using base_type::base_type;

    static constexpr partitioner_type type() noexcept { return partitioner_type::is_dynamic; }

    template<typename Fn>
    void loop(const stl::size_t total, const stl::size_t workers, stl::atomic<stl::size_t> &cursor,
              Fn &&work) const {
        const auto chunk = this->chunk_size != 0u ? this->chunk_size : stl::size_t{1};
        const auto fine_threshold = 2u * workers * (chunk + 1u);
        const auto shrink = 0.5f / static_cast<float>(workers);

        auto begin = cursor.load(stl::memory_order_relaxed);

        while(begin < total) {
            const auto remaining = total - begin;

            if(remaining < fine_threshold) {
                for(;;) {
                    begin = cursor.fetch_add(chunk, stl::memory_order_relaxed);

                    if(begin >= total) {
                        return;
                    }

                    const auto end = stl::min(begin + chunk, total);
                    this->wrap([&] { work(begin, end); });
                }
            }

            auto take = static_cast<stl::size_t>(shrink * static_cast<float>(remaining));
            take = take < chunk ? chunk : take;
            const auto end = stl::min(begin + take, total);

            // CAS rather than fetch_add: the size of the claim depends on what
            // is left, so it has to be computed from a value and then committed
            // against that same value.
            if(cursor.compare_exchange_strong(begin, end, stl::memory_order_relaxed,
                                              stl::memory_order_relaxed)) {
                this->wrap([&] { work(begin, end); });
                begin = end;
            }
        }
    }
};

/**
 * Randomly sized chunks between two fractions of the total.
 *
 * Not a joke: when items have wildly variable cost and the *pattern* of that
 * cost is adversarial to a fixed chunking (say, every 8th item is expensive and
 * the chunk size is 8), randomising the boundaries stops the pathology from
 * lining up with the schedule.
 */
template<typename Closure = no_closure>
class random_partitioner: public partitioner_base<Closure> {
    using base_type = partitioner_base<Closure>;

public:
    using base_type::base_type;

    random_partitioner() = default;

    random_partitioner(const float low, const float high)
        : base_type{}, alpha{low}, beta{high} {}

    static constexpr partitioner_type type() noexcept { return partitioner_type::is_dynamic; }

    template<typename Fn>
    void loop(const stl::size_t total, const stl::size_t workers, stl::atomic<stl::size_t> &cursor,
              Fn &&work) const {
        // Per-worker generator, so no shared RNG state and no contention.
        std::mt19937 random{static_cast<unsigned>(workers * 2654435761u + total)};
        const auto lower = static_cast<stl::size_t>(alpha * static_cast<float>(total));
        const auto upper = static_cast<stl::size_t>(beta * static_cast<float>(total));
        std::uniform_int_distribution<stl::size_t> pick{lower < 1u ? 1u : lower, upper < 2u ? 2u : upper};

        for(;;) {
            const auto take = pick(random);
            const auto begin = cursor.fetch_add(take, stl::memory_order_relaxed);

            if(begin >= total) {
                return;
            }

            const auto end = stl::min(begin + take, total);
            this->wrap([&] { work(begin, end); });
        }
    }

private:
    float alpha{0.01f};
    float beta{0.5f};
};

/** The default. A template parameter with a default costs the common case nothing. */
using default_partitioner = guided_partitioner<>;

} // namespace acpp

#endif // ACPP_PARTITIONER_HPP
