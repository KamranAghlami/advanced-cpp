#ifndef ACPP_ALGORITHM_HPP
#define ACPP_ALGORITHM_HPP

// Module 12 -- parallel algorithm skeletons over the partitioner policies.
//
// The skeleton branches on ONE thing: the partitioner's scheduling contract.
// `is_static` means pre-assigned ranges and no shared state; `is_dynamic` means
// a shared cursor. The chunk-sizing strategy never appears here -- that is the
// point of the enum/class split in partitioner.hpp.
//
// The partitioner is a template parameter WITH A DEFAULT, so the common case
// costs nothing at the call site and the specialist case needs no change to
// this file.

#include "executor.hpp"
#include "partitioner.hpp"
#include "stl/atomic.hpp"
#include "stl/cstddef.hpp"
#include "stl/functional.hpp"
#include "stl/type_traits.hpp"
#include "stl/utility.hpp"
#include "stl/vector.hpp"

namespace acpp {

/**
 * `for(i in [begin, end)) body(i)`, in parallel, inside a runtime task.
 *
 * Runs on the calling worker plus its peers via silent_async, and joins through
 * corun -- so a for_each nested inside another task cannot deadlock the pool.
 */
template<typename Index, typename Body, typename Partitioner = default_partitioner>
void for_each_index(runtime &rt, const Index begin, const Index end, Body body,
                    const Partitioner &part = Partitioner{}) {
    if(begin >= end) {
        return;
    }

    const auto total = static_cast<stl::size_t>(end - begin);
    const auto workers = static_cast<stl::size_t>(rt.owner().num_workers());

    const auto apply = [body, begin](const stl::size_t from, const stl::size_t to) {
        for(auto pos = from; pos < to; ++pos) {
            body(begin + static_cast<Index>(pos));
        }
    };

    if constexpr(Partitioner::type() == partitioner_type::is_static) {
        // Pre-assigned ranges: no shared state at all, so nothing to
        // synchronise and nothing to contend on.
        for(stl::size_t worker = 0u; worker < workers; ++worker) {
            rt.silent_async([=, &part] { part.loop(total, workers, worker, apply); });
        }
    } else {
        // One shared cursor, pulled by everybody. It has to outlive the tasks,
        // so it is heap-allocated and freed after the join below.
        auto cursor = stl::make_unique<stl::atomic<stl::size_t>>(0u);
        auto *raw = cursor.get();

        for(stl::size_t worker = 0u; worker < workers; ++worker) {
            rt.silent_async([=, &part] { part.loop(total, workers, *raw, apply); });
        }

        rt.corun();
        return;
    }

    rt.corun();
}

/** A parallel reduce with a per-worker partial, combined once at the end. */
template<typename Index, typename Type, typename Map, typename Reduce,
         typename Partitioner = default_partitioner>
Type reduce_index(runtime &rt, const Index begin, const Index end, Type initial, Map map, Reduce reduce,
                  const Partitioner &part = Partitioner{}) {
    if(begin >= end) {
        return initial;
    }

    const auto total = static_cast<stl::size_t>(end - begin);
    const auto workers = static_cast<stl::size_t>(rt.owner().num_workers());

    // One slot per worker, padded apart. Reducing into a shared atomic instead
    // would serialise the whole thing on one cache line -- the classic way to
    // write a parallel reduce that is slower than the serial one.
    struct alignas(ACPP_CACHELINE_SIZE) partial {
        Type value;
    };

    stl::vector<partial> partials(workers, partial{initial});
    auto cursor = stl::make_unique<stl::atomic<stl::size_t>>(0u);
    auto *raw = cursor.get();

    for(stl::size_t worker = 0u; worker < workers; ++worker) {
        auto *slot = &partials[worker];

        rt.silent_async([=, &part] {
            const auto apply = [&](const stl::size_t from, const stl::size_t to) {
                for(auto pos = from; pos < to; ++pos) {
                    slot->value = reduce(slot->value, map(begin + static_cast<Index>(pos)));
                }
            };

            if constexpr(Partitioner::type() == partitioner_type::is_static) {
                part.loop(total, workers, worker, apply);
            } else {
                part.loop(total, workers, *raw, apply);
            }
        });
    }

    rt.corun();

    auto result = initial;
    for(const auto &slot: partials) {
        result = reduce(result, slot.value);
    }

    return result;
}

} // namespace acpp

#endif // ACPP_ALGORITHM_HPP
