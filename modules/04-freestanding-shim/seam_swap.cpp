// Module 4, exercises 2 and 3 -- the seam, exercised from both sides.
//
// This TU is compiled TWICE by this directory's CMakeLists.txt:
//
//   seam_default   nothing on the include path, so src/acpp/stl/vector.hpp
//                  takes its own branch and acpp::stl::vector is std::vector
//   seam_fixed     modules/04-freestanding-shim/ext on the include path, so
//                  __has_include finds acpp/ext/stl/vector.hpp and
//                  acpp::stl::vector is a heap-free fixed-capacity vector
//
// Not one line of src/ differs between the two builds, and there is no build
// flag selecting behaviour -- only a header that either is or is not findable.
// That is the whole claim of the module, and running both is what checks it.

#include <acpp/ring_buffer.hpp>
#include <acpp/stl/cstddef.hpp>
#include <acpp/stl/utility.hpp>
#include <acpp/stl/vector.hpp>
#include <acpp/testing.hpp>

namespace {

// Library-shaped code, written once against the seam.
template<typename Type>
class journal {
public:
    void record(Type value) { entries.push_back(acpp::stl::move(value)); }

    [[nodiscard]] acpp::stl::size_t size() const noexcept { return entries.size(); }
    [[nodiscard]] const Type &operator[](const acpp::stl::size_t pos) const noexcept { return entries[pos]; }

    void rewind() { entries.clear(); }

private:
    acpp::stl::vector<Type> entries;
};

// Which implementation did we get? The test reports it rather than assuming,
// because a seam that silently failed to engage looks exactly like one that
// worked.
#if defined ACPP_EXT_STL_VECTOR_HPP
constexpr const char *flavour = "acpp/ext/stl/vector.hpp (fixed capacity, no heap)";
constexpr bool heap_free = true;
#else
constexpr const char *flavour = "<vector> (std::vector)";
constexpr bool heap_free = false;
#endif

} // namespace

int main() {
    acpp::testing::suite suite{"module 04 / seam_swap"};

    suite.note("acpp::stl::vector resolves to %s", flavour);

    journal<int> log;
    for(int i = 0; i < 100; ++i) {
        log.record(i * 3);
    }

    suite.check(log.size() == 100u, "the container works");
    suite.check(log[0] == 0 && log[99] == 297, "elements survive");

    log.rewind();
    suite.check(log.size() == 0u, "clear works");

    // Non-trivial elements must be constructed and destroyed properly by both
    // implementations. A fixed-capacity vector over aligned storage is exactly
    // where a missing destructor call hides.
    struct tracked {
        int *counter;
        explicit tracked(int *target) noexcept
            : counter{target} { ++*counter; }
        tracked(const tracked &other) noexcept
            : counter{other.counter} { ++*counter; }
        tracked(tracked &&other) noexcept
            : counter{other.counter} { ++*counter; }
        tracked &operator=(const tracked &) = delete;
        tracked &operator=(tracked &&) = delete;
        ~tracked() { --*counter; }
    };

    int live = 0;
    {
        journal<tracked> tracker;
        for(int i = 0; i < 20; ++i) {
            tracker.record(tracked{&live});
        }
        suite.check(live == 20, "20 elements alive");
    }

    suite.check(live == 0, "every element destroyed exactly once");

    // The seam does not reach the ring buffer's raw storage -- it allocates
    // through allocator_traits, not through a vector -- so it behaves the same
    // either way. Included to show the boundary of what was swapped.
    acpp::ring_buffer<int> ring{8u};
    for(int i = 0; i < 8; ++i) {
        (void)ring.push(i);
    }
    suite.check(ring.full(), "ring_buffer is unaffected by the vector swap");

    suite.check(heap_free == (flavour[0] == 'a'), "the flavour report is self-consistent");

    return suite.report();
}
