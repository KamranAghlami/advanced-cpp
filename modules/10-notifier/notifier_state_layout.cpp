// Module 10, exercise 1 -- the packed state word, drawn to scale and checked.
//
// The exercise says to draw it on paper before reading any of the functions.
// That drawing is in NOTES.md; this file is the version the compiler checks, so
// the drawing cannot drift from the code.

#include <cstdint>

#include <acpp/notifier.hpp>
#include <acpp/testing.hpp>

namespace {

using notifier = acpp::nonblocking_notifier;

// --- the three fields tile the word exactly, with no gaps and no overlap ----

static_assert(notifier::stack_bits + notifier::prewaiter_bits + notifier::epoch_bits == 64u,
              "the fields must tile a uint64 exactly");

static_assert(notifier::stack_mask == 0x000000000000FFFFull);
static_assert(notifier::prewaiter_mask == 0x00000000FFFF0000ull);
static_assert(notifier::epoch_mask == 0xFFFFFFFF00000000ull);

static_assert((notifier::stack_mask & notifier::prewaiter_mask) == 0u, "no overlap");
static_assert((notifier::prewaiter_mask & notifier::epoch_mask) == 0u, "no overlap");
static_assert((notifier::stack_mask | notifier::prewaiter_mask | notifier::epoch_mask)
                  == 0xFFFFFFFFFFFFFFFFull,
              "no gaps");

// The increments are one unit in the right field, which is what lets a single
// fetch_add or CAS move two fields at once (`state - PREWAITER_INC + EPOCH_INC`
// leaves the pre-wait stage and bumps the epoch in one operation).
static_assert(notifier::prewaiter_inc == (1ull << notifier::prewaiter_shift));
static_assert(notifier::epoch_inc == (1ull << notifier::epoch_shift));
static_assert((notifier::prewaiter_inc & notifier::prewaiter_mask) == notifier::prewaiter_inc);
static_assert((notifier::epoch_inc & notifier::epoch_mask) == notifier::epoch_inc);

// --- the maximum number of workers the encoding supports --------------------
//
// 16 stack bits gives 65,536 values, but the all-ones pattern is the
// "empty stack" sentinel, so it cannot also name a waiter. 65,535 indices
// remain, and the constructor refuses anything larger rather than aliasing a
// waiter with the sentinel -- which would be a hang, not a crash.
static_assert(notifier::max_waiters == 65534u);
static_assert(notifier::max_waiters < notifier::stack_mask);

// The pre-waiter count needs to hold every worker at once, since in the worst
// case all of them are between prepare_wait and commit_wait simultaneously.
static_assert(notifier::max_waiters <= (notifier::prewaiter_mask >> notifier::prewaiter_shift),
              "the pre-waiter field must be able to count every worker");

} // namespace

int main() {
    acpp::testing::suite suite{"module 10 / notifier_state_layout"};

    suite.note("state word: [0,16) stack  [16,32) prewaiters  [32,64) epoch");
    suite.note("stack_mask     = 0x%016llx  (sentinel: empty)",
               static_cast<unsigned long long>(notifier::stack_mask));
    suite.note("prewaiter_mask = 0x%016llx  (max %llu concurrent pre-waiters)",
               static_cast<unsigned long long>(notifier::prewaiter_mask),
               static_cast<unsigned long long>(notifier::prewaiter_mask >> notifier::prewaiter_shift));
    suite.note("epoch_mask     = 0x%016llx  (wraps every %llu requests)",
               static_cast<unsigned long long>(notifier::epoch_mask),
               static_cast<unsigned long long>(1ull << notifier::epoch_bits));
    suite.note("max_waiters    = %zu", notifier::max_waiters);

    suite.check(notifier::max_waiters == 65534u, "16 stack bits, minus the empty sentinel");

    // The constructor must refuse rather than alias. Cheap to check, and the
    // failure it prevents is a silent hang.
    bool refused = false;
    try {
        notifier oversized{notifier::max_waiters + 2u};
        (void)oversized;
    } catch(const std::length_error &) {
        refused = true;
    }

    suite.check(refused, "constructing past the encoding's capacity throws rather than aliasing");

    // A sanity check on the epoch's job: it is 32 bits, so it wraps. Wrapping is
    // harmless because every comparison is a *signed difference* of two epochs
    // that are close together, and unsigned subtraction reinterpreted as signed
    // gives the right answer as long as the true gap fits the signed range --
    // which it does, because the gap is bounded by the number of workers.
    const std::uint64_t near_wrap = 0xFFFFFFFF00000000ull;
    const std::uint64_t just_past = near_wrap + notifier::epoch_inc; // wraps to 0
    suite.check(static_cast<std::int64_t>(just_past - near_wrap) > 0,
                "epoch comparison survives wraparound via signed difference");

    return suite.report();
}
