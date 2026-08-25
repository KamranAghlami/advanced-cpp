// Module 7, exercise 1 -- typed storage with a paged payload, and the proof
// that addresses are stable.
//
// Checkpoint: paged sparse and paged payload exist for *different* reasons.
//
//   sparse   indexed by entity id, which is sparse. Flat would waste memory in
//            proportion to the largest id. Paging bounds the waste to one page.
//            Accepts: one indirection per lookup.
//
//   payload  indexed densely by packed position, so memory is not the problem.
//            Paging is for POINTER STABILITY: pages are never reallocated, so an
//            element's address never changes. A flat std::vector would move
//            every element on growth. Accepts: one indirection per access, and
//            iteration has to be page-aware.

#include <cstdint>
#include <string>
#include <vector>

#include <acpp/storage.hpp>
#include <acpp/testing.hpp>

namespace {

enum class entity : std::uint32_t {};

using traits = acpp::handle_traits<entity>;

[[nodiscard]] entity make(const std::uint32_t index) noexcept {
    return traits::construct(index, 0u);
}

struct position {
    float x, y;
};

// Not movable, so component_traits infers in_place_delete -- Module 2's
// correctness argument arriving where it is finally cashed in.
struct pinned {
    explicit pinned(int value) noexcept
        : id{value} {}

    pinned(const pinned &) = delete;
    pinned(pinned &&) = delete;
    pinned &operator=(const pinned &) = delete;
    pinned &operator=(pinned &&) = delete;
    ~pinned() = default;

    int id;
};

using position_storage = acpp::basic_storage<position, entity>;
using pinned_storage = acpp::basic_storage<pinned, entity>;

static_assert(position_storage::storage_policy == acpp::deletion_policy::swap_and_pop);
static_assert(pinned_storage::storage_policy == acpp::deletion_policy::in_place,
              "a non-movable type cannot be swap-and-popped; the policy is derived, not chosen");
static_assert(position_storage::page_size == ACPP_PACKED_PAGE);

} // namespace

int main() {
    acpp::testing::suite suite{"module 07 / paged_payload"};

    {
        position_storage storage;

        for(std::uint32_t i = 0u; i < 10u; ++i) {
            storage.emplace(make(i), position{static_cast<float>(i), 0.0f});
        }

        suite.check(storage.size() == 10u, "ten elements");
        suite.check(storage.get(make(7u)).x == 7.0f, "payload is reachable by entity");

        storage.get(make(7u)).y = 42.0f;
        suite.check(storage.get(make(7u)).y == 42.0f, "and writable");

        // The seam keeps the payload in step with the entity array.
        storage.erase(make(3u));
        suite.check(storage.size() == 9u, "erased");
        suite.check(!storage.contains(make(3u)), "gone");
        suite.check(storage.get(make(9u)).x == 9.0f,
                    "the element swapped into the hole brought its payload with it");
    }

    // --- pointer stability, the exercise's specific claim --------------------
    //
    // Hold a T*, insert 10,000 more elements, dereference it. Under a flat
    // std::vector this is a use-after-free; under paged storage it is fine,
    // because pages are allocated once and never moved.
    {
        position_storage storage;

        storage.emplace(make(0u), position{1.0f, 2.0f});
        position *held = &storage.get(make(0u));

        // Well past several page boundaries.
        for(std::uint32_t i = 1u; i <= 10'000u; ++i) {
            storage.emplace(make(i), position{static_cast<float>(i), 0.0f});
        }

        suite.check(held == &storage.get(make(0u)), "the address did not change after 10,000 inserts");
        suite.check(held->x == 1.0f && held->y == 2.0f, "and the object is intact");
        suite.check(storage.size() == 10'001u, "the storage really did grow");

        // Every element's address is stable, not just the first.
        std::vector<position *> addresses;
        for(std::uint32_t i = 0u; i < 100u; ++i) {
            addresses.push_back(&storage.get(make(i)));
        }

        for(std::uint32_t i = 10'001u; i <= 20'000u; ++i) {
            storage.emplace(make(i), position{static_cast<float>(i), 0.0f});
        }

        bool all_stable = true;
        for(std::uint32_t i = 0u; i < 100u; ++i) {
            all_stable = all_stable && (addresses[i] == &storage.get(make(i)));
        }

        suite.check(all_stable, "100 held addresses all survived another 10,000 inserts");
    }

    // --- and what swap_and_pop still costs ----------------------------------
    //
    // Pointer stability under *growth* is not pointer stability under *erasure*.
    // swap_and_pop moves the last element into the hole, so its address changes.
    // That is precisely why a type that needs stable addresses must opt into
    // in_place -- or, as with `pinned`, be inferred into it.
    {
        position_storage storage;
        for(std::uint32_t i = 0u; i < 5u; ++i) {
            storage.emplace(make(i), position{static_cast<float>(i), 0.0f});
        }

        const position *last = &storage.get(make(4u));
        storage.erase(make(1u));

        suite.check(last != &storage.get(make(4u)),
                    "under swap_and_pop, erasure DOES move an element's address");
    }

    // --- in_place: stable through erasure too -------------------------------
    {
        pinned_storage storage;
        for(std::uint32_t i = 0u; i < 20u; ++i) {
            storage.emplace(make(i), static_cast<int>(i));
        }

        const pinned *held = &storage.get(make(19u));

        storage.erase(make(5u));
        storage.erase(make(9u));

        suite.check(held == &storage.get(make(19u)), "in_place keeps addresses stable through erasure");
        suite.check(held->id == 19, "and the object is intact");
        suite.check(storage.size() == 20u, "the packed array kept its holes");
        suite.check(storage.count() == 18u, "with 18 live elements");
    }

    // --- destruction is exact ----------------------------------------------
    //
    // Paged raw storage is exactly where a missing or duplicated destructor call
    // hides, because the pages hold uninitialised bytes outside the live range.
    {
        struct counted {
            int *live;
            explicit counted(int *target) noexcept
                : live{target} { ++*live; }
            counted(const counted &other) noexcept
                : live{other.live} { ++*live; }
            counted(counted &&other) noexcept
                : live{other.live} { ++*live; }
            counted &operator=(const counted &) = default;
            counted &operator=(counted &&) = default;
            ~counted() { --*live; }
        };

        int live = 0;
        {
            acpp::basic_storage<counted, entity> storage;

            for(std::uint32_t i = 0u; i < 5000u; ++i) {
                storage.emplace(make(i), &live);
            }

            suite.check(live == 5000, "5000 elements constructed across several pages");

            for(std::uint32_t i = 0u; i < 2000u; ++i) {
                storage.erase(make(i));
            }

            suite.check(live == 3000, "2000 destroyed exactly once each");
        }

        suite.check(live == 0, "and the destructor released the rest");
    }

    return suite.report();
}
