// Module 9, exercise 5 -- the work-stealing deque against a mutex-guarded deque.
//
// Read the caveat before the numbers. This machine has ONE core.
// docs/CLAUDE.md says it plainly: steal-rate, scaling and idle-power
// measurements are meaningless on one core, and the honest thing is to report
// nproc alongside anything measured here.
//
// What one core CAN still show, and what this program is therefore for:
//   * the uncontended cost of the two protocols (atomics vs a lock), which is
//     what the owner pays on every push and pop even with no thieves at all;
//   * the cost of contention as *oversubscription* rather than parallelism --
//     N threads time-slicing one core, where the mutex version's blocked
//     waiters are descheduled and the lock-free version's are not.
//
// The scaling curve the exercise asks for needs a multi-core machine. Not faked.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include <acpp/wsq.hpp>

namespace {

constexpr int items_per_run = 400'000;

/** The obvious alternative: one lock around a std::deque. */
class locked_deque {
public:
    void push(int *item) {
        const std::lock_guard guard{mutex};
        items.push_back(item);
    }

    [[nodiscard]] int *pop() {
        const std::lock_guard guard{mutex};
        if(items.empty()) {
            return nullptr;
        }
        auto *item = items.back();
        items.pop_back();
        return item;
    }

    [[nodiscard]] int *steal() {
        const std::lock_guard guard{mutex};
        if(items.empty()) {
            return nullptr;
        }
        auto *item = items.front();
        items.pop_front();
        return item;
    }

    [[nodiscard]] bool empty() {
        const std::lock_guard guard{mutex};
        return items.empty();
    }

private:
    std::mutex mutex;
    std::deque<int *> items;
};

std::atomic<std::uint64_t> sink{0};

template<typename Queue>
[[nodiscard]] double run(Queue &queue, std::vector<int> &items, const unsigned thieves) {
    std::atomic<bool> producing{true};
    std::atomic<int> consumed{0};
    std::vector<std::thread> workers;

    const auto started = std::chrono::steady_clock::now();

    for(unsigned id = 0u; id < thieves; ++id) {
        workers.emplace_back([&] {
            std::uint64_t local = 0u;
            int taken = 0;

            while(producing.load(std::memory_order_relaxed) || !queue.empty()) {
                if(auto *item = queue.steal(); item != nullptr) {
                    local += static_cast<std::uint64_t>(*item);
                    ++taken;
                } else {
                    std::this_thread::yield();
                }
            }

            consumed.fetch_add(taken, std::memory_order_relaxed);
            sink.fetch_add(local, std::memory_order_relaxed);
        });
    }

    std::uint64_t local = 0u;
    int taken = 0;

    for(int i = 0; i < items_per_run; ++i) {
        queue.push(&items[static_cast<std::size_t>(i)]);

        if((i % 3) == 0) {
            if(auto *item = queue.pop(); item != nullptr) {
                local += static_cast<std::uint64_t>(*item);
                ++taken;
            }
        }
    }

    while(auto *item = queue.pop()) {
        local += static_cast<std::uint64_t>(*item);
        ++taken;
    }

    producing.store(false, std::memory_order_relaxed);

    for(auto &worker: workers) {
        worker.join();
    }

    while(auto *item = queue.pop()) {
        local += static_cast<std::uint64_t>(*item);
        ++taken;
    }

    const auto finished = std::chrono::steady_clock::now();

    consumed.fetch_add(taken, std::memory_order_relaxed);
    sink.fetch_add(local, std::memory_order_relaxed);

    if(consumed.load() != items_per_run) {
        std::printf("  !! consumed %d of %d -- the benchmark is not measuring what it claims\n",
                    consumed.load(), items_per_run);
    }

    return std::chrono::duration<double, std::milli>{finished - started}.count();
}

} // namespace

int main() {
    std::vector<int> items(items_per_run);
    for(int i = 0; i < items_per_run; ++i) {
        items[static_cast<std::size_t>(i)] = i;
    }

    const auto cores = std::thread::hardware_concurrency();

    std::printf("%d items, hardware_concurrency() = %u\n", items_per_run, cores);
    if(cores < 2u) {
        std::printf("ONE CORE: the columns below are oversubscription, not parallelism.\n"
                    "A scaling curve needs a multi-core machine; these numbers do not\n"
                    "support any claim about steal rate or throughput scaling.\n");
    }
    std::printf("\n%-10s %14s %14s %10s\n", "threads", "chase-lev (ms)", "mutex (ms)", "ratio");

    for(const unsigned thieves: {0u, 1u, 3u, 7u}) {
        acpp::unbounded_wsq<int *> lock_free{10u};
        locked_deque locked;

        const auto lock_free_ms = run(lock_free, items, thieves);
        const auto locked_ms = run(locked, items, thieves);

        std::printf("%-10u %14.2f %14.2f %10.2fx\n",
                    thieves + 1u, lock_free_ms, locked_ms, locked_ms / lock_free_ms);
    }

    return 0;
}
