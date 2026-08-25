// Module 8, exercise 2 -- small_any with the single-function vtable.
//
// Two design points under test:
//   * the SBO decision is made at compile time, per type, per <Len, Align>;
//   * ownership mode (value / ref / cref) is a separate axis from the type, so
//     one wrapper covers all three instead of three wrapper types.
//
// The nm half of the exercise ("verify only the operations you use get emitted")
// is a separate target -- see any_emission.cpp and the symbols_* tests.

#include <any>
#include <cstdint>
#include <string>
#include <type_traits>
#include <vector>

#include <acpp/any.hpp>
#include <acpp/testing.hpp>

namespace {

struct small {
    int a, b;
};

struct big {
    double values[8];
};

// Movable but not *nothrow* movable: pushed to the heap on purpose. Relocating
// an embedded object means moving the buffer, and a throwing move mid-relocation
// leaves two half-objects and no way back.
struct throwing_move {
    throwing_move() = default;
    throwing_move(const throwing_move &) = default;
    throwing_move(throwing_move &&) noexcept(false) {}
    throwing_move &operator=(const throwing_move &) = default;
    throwing_move &operator=(throwing_move &&) noexcept(false) { return *this; }
    ~throwing_move() = default;

    int value{};
};

using any = acpp::any;

// Buffer (16) + vtable pointer (8) + policy byte, rounded to alignment.
static_assert(sizeof(any) == sizeof(double[2]) + 2u * sizeof(void *));

// Len == 0 specialises the buffer away, leaving a pointer, a vtable pointer and
// the policy byte -- which then costs a whole word to padding. Worth noticing
// rather than glossing: it is exactly the situation Module 11 answers by packing
// small state into spare bits of a word that is already there. libstdc++'s
// std::any is 16 bytes because it manages with one pointer and one manager
// pointer and has no third field.
static_assert(sizeof(acpp::shallow_any) == 3u * sizeof(void *));

// NOT asserted against sizeof(std::any). That comparison was measuring the
// standard library, not this code: libstdc++'s any is 16 bytes and libc++'s is
// 32, for reasons to do with their small-buffer sizes and nothing to do with
// our policy field. The claim worth making is the absolute one above -- three
// words, one of which exists only to hold a byte -- and the comparison belongs
// in the output, where a reader can see which library they are on.

int live_counter = 0;

struct counted {
    counted() noexcept { ++live_counter; }
    counted(const counted &) noexcept { ++live_counter; }
    counted(counted &&) noexcept { ++live_counter; }
    counted &operator=(const counted &) = default;
    counted &operator=(counted &&) = default;
    ~counted() { --live_counter; }

    int padding[4]{}; // still fits the buffer
};

} // namespace

int main() {
    acpp::testing::suite suite{"module 08 / small_any"};

    suite.note("sizeof(shallow_any) = %zu, sizeof(std::any) = %zu (standard-library dependent)",
               sizeof(acpp::shallow_any), sizeof(std::any));

    // --- the SBO decision ---------------------------------------------------
    {
        any inline_value{small{1, 2}};
        any heap_value{big{}};
        any thrower{throwing_move{}};

        suite.check(inline_value.policy() == acpp::any_policy::embedded, "a small type is embedded");
        suite.check(heap_value.policy() == acpp::any_policy::dynamic, "a large type goes to the heap");
        suite.check(thrower.policy() == acpp::any_policy::dynamic,
                    "so does a movable-but-throwing type, so relocation cannot fail half way");

        suite.check(inline_value.type() == acpp::type_id<small>(), "the type is recoverable");
        suite.check(acpp::any_cast<small>(&inline_value)->b == 2, "and the value with it");
        suite.check(acpp::any_cast<big>(&inline_value) == nullptr, "a wrong-type cast returns null");
    }

    // --- ownership mode as a separate axis ----------------------------------
    {
        int value = 7;
        any owner{value};
        any alias = owner.as_ref();
        any const_alias = owner.as_cref();

        suite.check(owner.owner(), "a value-constructed any owns");
        suite.check(!alias.owner() && !const_alias.owner(), "an alias does not");
        suite.check(alias.type() == acpp::type_id<int>(), "an alias reports the aliased type");

        *static_cast<int *>(alias.data()) = 9;
        suite.check(acpp::any_cast<int>(owner) == 9, "writing through a ref alias reaches the original");

        // A cref alias must not hand back a mutable pointer -- laundering the
        // const away here would be the one place this design could go wrong
        // silently.
        suite.check(const_alias.data() == nullptr, "a cref alias refuses mutable access");
        suite.check(acpp::any_cast<int>(const_alias) == 9, "but reads fine");

        // Copying an alias copies the alias, not the object. That is the
        // aliasing contract, and it is easy to get backwards.
        any copied_alias{alias};
        suite.check(!copied_alias.owner(), "copying an alias yields an alias");
    }

    // --- lifetimes ----------------------------------------------------------
    {
        live_counter = 0;
        {
            any a{counted{}};
            suite.check(live_counter == 1, "one live object");

            any b{a};
            suite.check(live_counter == 2, "copy constructs a second");

            any c{std::move(b)};
            suite.check(live_counter == 2, "move does not construct a third");

            a.reset();
            suite.check(live_counter == 1, "reset destroys");
        }

        suite.check(live_counter == 0, "and the destructor cleans up the rest");

        // An alias must never destroy what it does not own.
        live_counter = 0;
        {
            any owned{counted{}};
            {
                any alias = owned.as_ref();
                (void)alias;
            }
            suite.check(live_counter == 1, "an alias going out of scope destroys nothing");
        }
        suite.check(live_counter == 0, "the owner still cleans up");
    }

    // --- comparison ---------------------------------------------------------
    {
        suite.check(any{42} == any{42}, "equal values compare equal");
        suite.check(!(any{42} == any{43}), "unequal values do not");
        suite.check(!(any{42} == any{42.0}), "and different types never do");
        suite.check(any{} == any{}, "two empty anys are equal");
        suite.check(!(any{} == any{1}), "empty is not equal to non-empty");

        // A type with no operator== falls back to identity, which is the only
        // answer that is never wrong.
        any a{big{}};
        any b{big{}};
        suite.check(!(a == b), "types without operator== compare by identity");
        suite.check(a == a.as_ref(), "and an alias to the same object is identical");
    }

    // --- heap-only variant --------------------------------------------------
    {
        acpp::shallow_any value{small{3, 4}};
        suite.check(value.policy() == acpp::any_policy::dynamic,
                    "with no buffer, even a small type is heap allocated");
        suite.check(acpp::any_cast<small>(&value)->a == 3, "and still works");
    }

    // --- a container of heterogeneous values --------------------------------
    {
        std::vector<any> values;
        values.emplace_back(1);
        values.emplace_back(std::string{"two"});
        values.emplace_back(3.0);

        suite.check(values[0].type() == acpp::type_id<int>(), "int");
        suite.check(values[1].type() == acpp::type_id<std::string>(), "string");
        suite.check(acpp::any_cast<std::string>(values[1]) == "two", "value survives the vector's reallocation");
    }

    return suite.report();
}
