// Module 4, exercise 2 -- the seam, used against EnTT itself.
//
// entt_ext/entt/ext/stl/vector.hpp replaces entt::stl::vector with a
// fixed-capacity, heap-free vector. EnTT's own stl/vector.hpp finds it through
// __has_include; nothing else changes, and no source in third_party/ is touched.
//
// The exercise predicts this will not get all of EnTT compiling and that
// locating the break is the point. The measured answer here is that ALL of
// entt.hpp compiles and runs -- meta/ included -- once the replacement supplies
// 13 members and 8 typedefs. NOTES.md has the list and how it was found.
//
// Including <entt/entt.hpp> rather than a curated subset is deliberate: the
// claim is "all of it", so the test has to compile all of it.

#include <cstddef>
#include <cstdlib>

// Must come first so the substitution is in place before anything uses it.
#include <entt/stl/vector.hpp>

#include <entt/entt.hpp>

#include <acpp/testing.hpp>

namespace {

// Counts every trip to the global allocator, so the module's claim can be
// checked rather than asserted. Swapping the vector removes *its* allocations;
// it says nothing about the ones sparse_set makes for its pages through
// allocator_traits, and conflating the two would be exactly the kind of
// hand-wave a freestanding port cannot afford.
std::size_t allocations = 0u;

struct empty_tag {};

struct position {
    float x, y;
};

struct velocity {
    float dx, dy;
};

// Proof the replacement is what is in play, rather than <vector> sneaking back
// in through some other include. static_capacity exists only on ours.
static_assert(entt::stl::vector<int>::static_capacity > 0u);

// Module 3's technique, inside the library, on the replacement container.
static_assert(sizeof(entt::compressed_pair<empty_tag, int>) == sizeof(int));

} // namespace

// Replacing the global allocation functions collides with the sanitizer
// runtimes, which define their own to track allocations -- the link fails with
// "multiple definition of operator new". They own this hook; we borrow it only
// when they are not present, and say so in the output rather than silently
// reporting zero.
#if defined __SANITIZE_ADDRESS__ || defined __SANITIZE_THREAD__
#    define ACPP_SANITIZED 1
#elif defined __has_feature
#    if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || __has_feature(memory_sanitizer)
#        define ACPP_SANITIZED 1
#    endif
#endif

#if !defined ACPP_SANITIZED
void *operator new(std::size_t bytes) {
    ++allocations;
    return std::malloc(bytes);
}

void operator delete(void *pointer) noexcept { std::free(pointer); }
void operator delete(void *pointer, std::size_t) noexcept { std::free(pointer); }
#endif

int main() {
    acpp::testing::suite suite{"module 04 / entt_on_fixed_vector"};

    suite.note("entt::stl::vector<T>::static_capacity = %zu", entt::stl::vector<int>::static_capacity);

    // The registry is the whole stack: sparse sets, paged storage, views,
    // generational handles -- Modules 5 through 7, running heap-free.
    entt::registry registry;

    const auto before = allocations;

    for(int i = 0; i < 32; ++i) {
        const auto entity = registry.create();
        registry.emplace<position>(entity, static_cast<float>(i), 0.0f);

        if(i % 2 == 0) {
            registry.emplace<velocity>(entity, 1.0f, 0.0f);
        }
    }

    int moved = 0;
    for(auto [entity, pos, vel]: registry.view<position, velocity>().each()) {
        pos.x += vel.dx;
        ++moved;
    }

    suite.check(moved == 16, "registry, storage and views all work on the replacement vector");

    // The honest version of the claim. The vector no longer allocates; the
    // storage's paged arrays still go through allocator_traits and do.
#if defined ACPP_SANITIZED
    (void)before;
    suite.note("allocation counting is disabled under sanitizers (the runtime owns operator new)");
#else
    suite.note("global allocations during 32 creates + 48 emplaces + a view pass: %zu",
               allocations - before);
#endif

    const auto doomed = registry.create();
    registry.destroy(doomed);
    suite.check(!registry.valid(doomed), "generational handles still detect staleness");

    // meta/ is the part that needed cbegin/cend, and the last thing to give way.
    using namespace entt::literals;
    entt::meta_factory<position>{}.type("position"_hs).data<&position::x>("x"_hs);

    auto resolved = entt::resolve("position"_hs);
    suite.check(static_cast<bool>(resolved), "runtime reflection resolves a type");
    position sample{3.0f, 4.0f};
    suite.check(resolved.data("x"_hs).get(sample).cast<float>() == 3.0f,
                "and reads a member through it, with the reflection tables in fixed vectors");

    return suite.report();
}
