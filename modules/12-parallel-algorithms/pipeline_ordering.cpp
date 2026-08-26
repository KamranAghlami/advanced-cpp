// Module 12, exercises 3 and 4 -- a three-stage pipeline, and the closure
// wrapper.
//
// Stage shape: serial -> parallel -> serial. The property that has to hold is
// that output leaves the FINAL serial stage in token order, even though the
// middle stage processed tokens in whatever order it liked.

#include <atomic>
#include <cstdint>
#include <mutex>
#include <numeric>
#include <thread>
#include <vector>

#include <acpp/config.hpp>
#include <acpp/algorithm.hpp>
#include <acpp/pipeline.hpp>
#include <acpp/testing.hpp>

namespace {

constexpr std::size_t tokens = 200u;
constexpr std::size_t lines = 4u;

/**
 * Exercise 4's injection point: one scratch buffer per worker, not per item.
 *
 * At namespace scope, not inside main. A local class cannot have member
 * templates -- gcc accepts it as an extension, clang correctly refuses, and the
 * wrapper has to be a template because it wraps an arbitrary callable.
 */
struct scratch_wrapper {
    std::atomic<std::size_t> *counter;

    template<typename Fn>
    void operator()(Fn &&work) const {
        thread_local std::vector<long> scratch;

        if(scratch.empty()) {
            scratch.resize(8);
            counter->fetch_add(1u, std::memory_order_relaxed); // once per WORKER
        }

        work();
    }
};

} // namespace

int main() {
    acpp::testing::suite suite{"module 12 / pipeline_ordering"};

    acpp::executor pool{4u};
    suite.note("hardware_concurrency() = %u, %zu lines, %zu tokens",
               std::thread::hardware_concurrency(), lines, tokens);

    // --- serial -> parallel -> serial ---------------------------------------
    {
        // One slot per line: the in-flight state a token carries between
        // stages. Sizing this by lines rather than by tokens is the whole point
        // of a pipeline -- memory is bounded by depth, not by stream length.
        std::vector<long> buffer(lines, 0);

        std::mutex mutex;
        std::vector<long> output;
        std::atomic<std::size_t> produced{0u};
        std::atomic<std::size_t> transformed{0u};
        std::atomic<std::size_t> max_concurrent_middle{0u};
        std::atomic<std::size_t> in_middle{0u};

        acpp::pipeline stream{
            lines,
            {
                {acpp::pipe_type::serial,
                 [&](acpp::pipeflow &flow) {
                     if(flow.token() >= tokens) {
                         flow.stop();
                         return;
                     }

                     buffer[flow.line()] = static_cast<long>(flow.token());
                     produced.fetch_add(1u, std::memory_order_relaxed);
                 }},

                {acpp::pipe_type::parallel,
                 [&](acpp::pipeflow &flow) {
                     const auto now = in_middle.fetch_add(1u, std::memory_order_acq_rel) + 1u;

                     auto observed = max_concurrent_middle.load(std::memory_order_relaxed);
                     while(now > observed
                           && !max_concurrent_middle.compare_exchange_weak(observed, now,
                                                                           std::memory_order_relaxed)) {}

                     buffer[flow.line()] = buffer[flow.line()] * 2 + 1;
                     transformed.fetch_add(1u, std::memory_order_relaxed);

                     in_middle.fetch_sub(1u, std::memory_order_acq_rel);
                 }},

                {acpp::pipe_type::serial,
                 [&](acpp::pipeflow &flow) {
                     const std::lock_guard guard{mutex};
                     output.push_back(buffer[flow.line()]);
                 }},
            }};

        acpp::taskflow graph;
        graph.emplace_runtime([&](acpp::runtime &rt) { stream.run(rt); });
        pool.run(graph)->wait();

        suite.check(produced.load() == tokens, "the source produced every token exactly once");
        suite.check(transformed.load() == tokens, "the parallel stage saw every token exactly once");
        suite.check(output.size() == tokens, "the sink received every token exactly once");

        // THE property. The middle stage is unordered; the output is not.
        bool ordered = true;
        for(std::size_t i = 0u; i < output.size(); ++i) {
            if(output[i] != static_cast<long>(i) * 2 + 1) {
                ordered = false;
                break;
            }
        }

        suite.check(ordered, "output left the final serial stage in strict token order");
        suite.note("peak concurrency in the parallel stage: %zu", max_concurrent_middle.load());
    }

    // --- an all-serial pipeline --------------------------------------------
    //
    // The degenerate case, and the one that catches an ordering bug that a
    // parallel middle stage would hide.
    {
        std::vector<long> buffer(lines, 0);
        std::vector<long> seen_at_second;
        std::mutex mutex;

        acpp::pipeline stream{lines,
                              {
                                  {acpp::pipe_type::serial,
                                   [&](acpp::pipeflow &flow) {
                                       if(flow.token() >= 50u) {
                                           flow.stop();
                                           return;
                                       }
                                       buffer[flow.line()] = static_cast<long>(flow.token());
                                   }},
                                  {acpp::pipe_type::serial,
                                   [&](acpp::pipeflow &flow) {
                                       const std::lock_guard guard{mutex};
                                       seen_at_second.push_back(buffer[flow.line()]);
                                   }},
                              }};

        acpp::taskflow graph;
        graph.emplace_runtime([&](acpp::runtime &rt) { stream.run(rt); });
        pool.run(graph)->wait();

        bool ordered = seen_at_second.size() == 50u;
        for(std::size_t i = 0u; i < seen_at_second.size(); ++i) {
            ordered = ordered && seen_at_second[i] == static_cast<long>(i);
        }

        suite.check(ordered, "an all-serial pipeline preserves order at every stage");
    }

    // --- exercise 4: the closure wrapper ------------------------------------
    //
    // The injection point: a user wraps every chunk in their own setup and
    // teardown, and the algorithm never learns about it. Here it hands each
    // chunk a scratch buffer that is allocated once per worker rather than once
    // per chunk.
    {
        std::atomic<std::size_t> allocations{0u};
        std::atomic<long> total{0};

        // Without a wrapper: the body allocates its own scratch every chunk.
        {
            acpp::taskflow graph;
            graph.emplace_runtime([&](acpp::runtime &rt) {
                acpp::for_each_index(
                    rt, 0, 20000,
                    [&](const int i) {
                        std::vector<long> scratch(8, 0);   // one per ITEM
                        allocations.fetch_add(1u, std::memory_order_relaxed);
                        scratch[0] = i;
                        total.fetch_add(scratch[0], std::memory_order_relaxed);
                    },
                    acpp::dynamic_partitioner<>{256u});
            });
            pool.run(graph)->wait();
        }

        const auto without_wrapper = allocations.load();
        allocations.store(0u);
        total.store(0);

        // With a wrapper: one scratch buffer per worker, reused across every
        // chunk. The body is unchanged in shape; the wrapper owns the resource.
        {
            using wrapped = acpp::dynamic_partitioner<scratch_wrapper>;

            acpp::taskflow graph;
            graph.emplace_runtime([&](acpp::runtime &rt) {
                acpp::for_each_index(
                    rt, 0, 20000,
                    [&](const int i) { total.fetch_add(i, std::memory_order_relaxed); },
                    wrapped{256u, scratch_wrapper{&allocations}});
            });
            pool.run(graph)->wait();
        }

        const auto with_wrapper = allocations.load();

        suite.note("scratch allocations: %zu without the wrapper, %zu with it",
                   without_wrapper, with_wrapper);
        suite.check(with_wrapper < without_wrapper / 100u,
                    "the closure wrapper cut scratch allocations by more than 100x");
        suite.check(total.load() == 19999L * 20000 / 2, "and the result is still correct");

        // And the reason the default costs nothing: no_closure is empty, so
        // [[no_unique_address]] gives it no storage at all -- everywhere the
        // attribute is honoured. On MSVC it is ignored and the empty closure
        // costs a word, which is the same finding Module 3 records for
        // compressed_pair and the same reason EnTT keeps the EBO version.
        suite.note("dynamic_partitioner<> = %zu bytes (chunk size is %zu)",
                   sizeof(acpp::dynamic_partitioner<>), sizeof(std::size_t));
        suite.check(sizeof(acpp::dynamic_partitioner<>)
                        == (acpp::nua_compresses ? sizeof(std::size_t) : 2u * sizeof(std::size_t)),
                    "a partitioner with no closure costs only its chunk size");
    }

    return suite.report();
}
