// Module 10, exercise 5 -- what idling costs.
//
// Three ways for a worker with nothing to do to wait for work:
//
//   spin      never sleep. Lowest latency, and it burns a core.
//   2PC       park when there is nothing, wake on notify. One syscall per
//             sleep/wake pair, and no synchronisation on push when nobody
//             sleeps.
//   condvar   a mutex + condition_variable signalled on EVERY push, whether or
//             not anyone is asleep.
//
// The number that matters on a battery-powered target is CPU time consumed
// while idle, not wall time -- so this measures getrusage(), not the clock.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

#if defined _WIN32
#    include <windows.h>
#else
#    include <sys/resource.h>
#endif

#include <acpp/notifier.hpp>

namespace {

using namespace std::chrono_literals;

/**
 * CPU time consumed by this process, user + kernel.
 *
 * Wall time is useless here: the workload is mostly idle by construction, so
 * every strategy takes about the same wall time and they differ only in what
 * they burn while waiting. That is the whole measurement.
 */
[[nodiscard]] double cpu_seconds() {
#if defined _WIN32
    FILETIME created{};
    FILETIME exited{};
    FILETIME kernel{};
    FILETIME user{};
    GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);

    // FILETIME counts 100-nanosecond ticks, split across two 32-bit halves.
    const auto seconds = [](const FILETIME &value) {
        ULARGE_INTEGER packed{};
        packed.LowPart = value.dwLowDateTime;
        packed.HighPart = value.dwHighDateTime;
        return static_cast<double>(packed.QuadPart) * 1e-7;
    };

    return seconds(kernel) + seconds(user);
#else
    rusage usage{};
    getrusage(RUSAGE_SELF, &usage);

    const auto user = static_cast<double>(usage.ru_utime.tv_sec)
                      + static_cast<double>(usage.ru_utime.tv_usec) * 1e-6;
    const auto system = static_cast<double>(usage.ru_stime.tv_sec)
                        + static_cast<double>(usage.ru_stime.tv_usec) * 1e-6;
    return user + system;
#endif
}

struct result {
    double cpu_seconds;
    double wall_ms;
    int consumed;
    std::uint64_t wakeups;
};

// A bursty workload: a short burst of items, then a long idle gap. The gap is
// where the three strategies differ, and it is what a real event-driven system
// spends most of its life in.
constexpr int bursts = 12;
constexpr int per_burst = 40;
constexpr auto gap = 25ms;

/** Strategy 1: spin. */
[[nodiscard]] result run_spin(const unsigned workers) {
    std::mutex mutex;
    std::queue<int> items;
    std::atomic<bool> done{false};
    std::atomic<int> consumed{0};
    std::vector<std::thread> threads;

    const auto cpu_before = cpu_seconds();
    const auto started = std::chrono::steady_clock::now();

    for(unsigned id = 0u; id < workers; ++id) {
        threads.emplace_back([&] {
            for(;;) {
                int item = 0;
                bool got = false;
                {
                    const std::lock_guard guard{mutex};
                    if(!items.empty()) {
                        item = items.front();
                        items.pop();
                        got = true;
                    }
                }

                if(got) {
                    consumed.fetch_add(1, std::memory_order_relaxed);
                    (void)item;
                } else if(done.load(std::memory_order_acquire)) {
                    return;
                } else {
                    std::this_thread::yield(); // "spin", politely
                }
            }
        });
    }

    for(int burst = 0; burst < bursts; ++burst) {
        {
            const std::lock_guard guard{mutex};
            for(int i = 0; i < per_burst; ++i) {
                items.push(i);
            }
        }
        std::this_thread::sleep_for(gap);
    }

    done.store(true, std::memory_order_release);
    for(auto &thread: threads) {
        thread.join();
    }

    return {cpu_seconds() - cpu_before,
            std::chrono::duration<double, std::milli>{std::chrono::steady_clock::now() - started}.count(),
            consumed.load(), 0u};
}

/** Strategy 2: the two-phase notifier. */
template<typename Notifier>
[[nodiscard]] result run_two_phase(const unsigned workers) {
    Notifier notifier{workers};
    std::mutex mutex;
    std::queue<int> items;
    std::atomic<bool> done{false};
    std::atomic<int> consumed{0};
    std::atomic<std::uint64_t> parks{0u};
    std::vector<std::thread> threads;

    const auto has_work = [&] {
        const std::lock_guard guard{mutex};
        return !items.empty();
    };

    const auto cpu_before = cpu_seconds();
    const auto started = std::chrono::steady_clock::now();

    for(unsigned id = 0u; id < workers; ++id) {
        threads.emplace_back([&, id] {
            for(;;) {
                for(;;) {
                    int item = 0;
                    {
                        const std::lock_guard guard{mutex};
                        if(items.empty()) {
                            break;
                        }
                        item = items.front();
                        items.pop();
                    }
                    consumed.fetch_add(1, std::memory_order_relaxed);
                    (void)item;
                }

                notifier.prepare_wait(id);

                if(has_work()) {
                    notifier.cancel_wait(id);
                    continue;
                }

                if(done.load(std::memory_order_acquire)) {
                    notifier.cancel_wait(id);
                    return;
                }

                parks.fetch_add(1u, std::memory_order_relaxed);
                notifier.commit_wait(id);

                if(done.load(std::memory_order_acquire) && !has_work()) {
                    return;
                }
            }
        });
    }

    for(int burst = 0; burst < bursts; ++burst) {
        for(int i = 0; i < per_burst; ++i) {
            {
                const std::lock_guard guard{mutex};
                items.push(i);
            }
            notifier.notify_one();
        }
        std::this_thread::sleep_for(gap);
    }

    done.store(true, std::memory_order_release);
    notifier.notify_all();
    for(auto &thread: threads) {
        thread.join();
    }

    return {cpu_seconds() - cpu_before,
            std::chrono::duration<double, std::milli>{std::chrono::steady_clock::now() - started}.count(),
            consumed.load(), parks.load()};
}

/** Strategy 3: mutex + condvar, signalled on every push. */
[[nodiscard]] result run_condvar(const unsigned workers) {
    std::mutex mutex;
    std::condition_variable cv;
    std::queue<int> items;
    bool done = false;
    std::atomic<int> consumed{0};
    std::atomic<std::uint64_t> wakeups{0u};
    std::vector<std::thread> threads;

    const auto cpu_before = cpu_seconds();
    const auto started = std::chrono::steady_clock::now();

    for(unsigned id = 0u; id < workers; ++id) {
        threads.emplace_back([&] {
            for(;;) {
                std::unique_lock guard{mutex};
                cv.wait(guard, [&] { return !items.empty() || done; });

                if(items.empty()) {
                    return;
                }

                items.pop();
                guard.unlock();
                wakeups.fetch_add(1u, std::memory_order_relaxed);
                consumed.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    for(int burst = 0; burst < bursts; ++burst) {
        for(int i = 0; i < per_burst; ++i) {
            {
                const std::lock_guard guard{mutex};
                items.push(i);
            }
            cv.notify_one(); // every push, unconditionally
        }
        std::this_thread::sleep_for(gap);
    }

    {
        const std::lock_guard guard{mutex};
        done = true;
    }
    cv.notify_all();

    for(auto &thread: threads) {
        thread.join();
    }

    return {cpu_seconds() - cpu_before,
            std::chrono::duration<double, std::milli>{std::chrono::steady_clock::now() - started}.count(),
            consumed.load(), wakeups.load()};
}

void report(const char *name, const result &value) {
    const auto expected = bursts * per_burst;
    std::printf("%-26s %10.4f %10.1f %8d%s %10llu\n", name, value.cpu_seconds, value.wall_ms,
                value.consumed, value.consumed == expected ? " " : "!", 
                static_cast<unsigned long long>(value.wakeups));
}

} // namespace

int main() {
    const unsigned workers = 4u;

    std::printf("bursty load: %d bursts of %d items, %lld ms idle between them\n",
                bursts, per_burst, static_cast<long long>(gap.count()));
    std::printf("workers = %u, hardware_concurrency() = %u\n\n", workers,
                std::thread::hardware_concurrency());
    std::printf("%-26s %10s %10s %9s %10s\n", "strategy", "cpu (s)", "wall (ms)", "items", "parks");

    report("spin (yield loop)", run_spin(workers));
    report("2PC blocking_notifier", run_two_phase<acpp::blocking_notifier>(workers));
    report("2PC nonblocking_notifier", run_two_phase<acpp::nonblocking_notifier>(workers));
    report("condvar, signal on push", run_condvar(workers));

    std::printf("\nCPU seconds is the number that matters on a battery-powered target:\n"
                "wall time is dominated by the idle gaps and says almost nothing.\n");

    return 0;
}
