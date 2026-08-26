// Module 6, exercise 3 -- packed iteration vs std::unordered_map.
//
// Not a ctest entry: it is a measurement, not a pass/fail. Run it directly, or
// under perf/cachegrind:
//
//   ./build/modules/06-sparse-set/sparse_set_bench
//   perf stat -e cache-misses,cache-references ... sparse_set_bench 1000000 packed
//   valgrind --tool=cachegrind --D1=32768,8,64 ... sparse_set_bench 200000 map
//
// The optional second argument runs only one side, so a profiler's numbers are
// attributable to one data structure instead of to both plus the setup.
//
// The number that matters is the *ratio* at each size, and the explanation has
// to be in cache lines rather than vibes -- see NOTES.md.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <numeric>
#include <random>
#include <unordered_map>
#include <vector>

#include <acpp/sparse_set.hpp>

namespace {

enum class entity : std::uint32_t {};

using traits = acpp::handle_traits<entity>;

struct payload {
    float x, y, z, w;
};

[[nodiscard]] entity make(const std::uint32_t index) noexcept {
    return traits::construct(index, 0u);
}

// A derived storage with a flat parallel payload array: the shape Module 7 will
// make paged. Flat is the right comparison here -- it isolates the sparse-set
// idea from the paging decision.
class storage: public acpp::basic_sparse_set<entity> {
public:
    void emplace(const entity entt, const payload &value) {
        const auto pos = push(entt);
        if(pos >= values.size()) {
            values.resize(pos + 1u);
        }
        values[pos] = value;
    }

    [[nodiscard]] const std::vector<payload> &data() const noexcept { return values; }

private:
    void swap_or_move(const std::size_t lhs, const std::size_t rhs) override {
        std::swap(values[lhs], values[rhs]);
    }

    std::vector<payload> values;
};

template<typename Fn>
[[nodiscard]] double time_ms(Fn &&fn) {
    const auto started = std::chrono::steady_clock::now();
    fn();
    const auto finished = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>{finished - started}.count();
}

// Consumed so the optimiser cannot delete the loops it is supposed to time.
volatile float sink = 0.0f;

enum class which { both, packed_only, map_only };

void run(const std::size_t count, const which side = which::both) {
    // Ids are shuffled and sparse -- 4x spread -- so the map cannot win on a
    // lucky insertion order, and the sparse array's paging is actually used.
    std::vector<std::uint32_t> ids(count);
    std::iota(ids.begin(), ids.end(), 0u);
    for(auto &id: ids) {
        id *= 4u;
    }

    std::mt19937 random{20260826u};
    std::shuffle(ids.begin(), ids.end(), random);

    storage packed;
    std::unordered_map<std::uint32_t, payload> map;
    map.reserve(count);

    for(std::size_t i = 0u; i < count; ++i) {
        const payload value{static_cast<float>(i), 1.0f, 2.0f, 3.0f};
        packed.emplace(make(ids[i]), value);
        map.emplace(ids[i], value);
    }

    if(side != which::both) {
        // Profiler mode: one structure, one long iteration, nothing else in the
        // measurement window.
        const auto repeats = 64u;
        float total = 0.0f;

        for(unsigned pass = 0u; pass < repeats; ++pass) {
            if(side == which::packed_only) {
                for(const auto &value: packed.data()) {
                    total += value.x;
                }
            } else {
                for(const auto &[key, value]: map) {
                    total += value.x;
                }
            }
        }

        sink = total;
        std::printf("%s: %zu elements x %u passes\n",
                    side == which::packed_only ? "packed" : "map", count, repeats);
        return;
    }

    // Iterate everything and sum one field. The access pattern is the point:
    // the packed array walks contiguous memory; the map chases node pointers.
    const auto packed_ms = time_ms([&] {
        float total = 0.0f;
        for(const auto &value: packed.data()) {
            total += value.x;
        }
        sink = total;
    });

    const auto map_ms = time_ms([&] {
        float total = 0.0f;
        for(const auto &[key, value]: map) {
            total += value.x;
        }
        sink = total;
    });

    // Random lookup, for contrast: this is the operation the map is supposed to
    // be good at, and the one where the sparse set's advantage is smallest.
    std::vector<std::uint32_t> probes(ids);
    std::shuffle(probes.begin(), probes.end(), random);

    const auto packed_lookup_ms = time_ms([&] {
        float total = 0.0f;
        for(const auto id: probes) {
            total += packed.data()[packed.index(make(id))].x;
        }
        sink = total;
    });

    const auto map_lookup_ms = time_ms([&] {
        float total = 0.0f;
        for(const auto id: probes) {
            total += map.find(id)->second.x;
        }
        sink = total;
    });

    std::printf("%9zu | %8.3f %8.3f %6.1fx | %8.3f %8.3f %6.1fx\n",
                count,
                packed_ms, map_ms, map_ms / packed_ms,
                packed_lookup_ms, map_lookup_ms, map_lookup_ms / packed_lookup_ms);
}

} // namespace

int main(int argc, char **argv) {
    std::printf("sparse set vs std::unordered_map, payload = %zu bytes\n", sizeof(payload));
    std::printf("%9s | %8s %8s %7s | %8s %8s %7s\n",
                "elements", "packed", "map", "ratio", "pk-find", "map-find", "ratio");
    std::printf("          |   full iteration (ms)        |   random lookup (ms)\n");

    if(argc > 1) {
        auto side = which::both;
        if(argc > 2) {
            side = (argv[2][0] == 'p') ? which::packed_only : which::map_only;
        }
        run(static_cast<std::size_t>(std::strtoul(argv[1], nullptr, 10)), side);
        return 0;
    }

    for(const std::size_t count: {10'000u, 100'000u, 1'000'000u}) {
        run(count);
    }

    return 0;
}
