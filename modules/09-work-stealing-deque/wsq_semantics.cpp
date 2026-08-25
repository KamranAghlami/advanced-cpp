// Module 9 -- the single-threaded contract, and the one race isolated.
//
// Concurrency bugs are found by stress tests; concurrency *semantics* are
// pinned by single-threaded ones. Everything here runs on one thread, which is
// what makes a failure mean "the algorithm is wrong" rather than "the schedule
// was unlucky".

#include <cstdint>
#include <vector>

#include <acpp/testing.hpp>
#include <acpp/wsq.hpp>

namespace {

using queue = acpp::bounded_wsq<int *, 4u>; // 16 slots

static_assert(queue::capacity == 16);
static_assert(queue::mask == 15);

// The empty sentinel is in the return value, not an out-parameter: for a
// pointer element type it is nullptr, so `if(auto *task = q.pop())` reads the
// way a caller wants to write it.
static_assert(std::is_same_v<queue::value_type, int *>);
static_assert(std::is_same_v<acpp::bounded_wsq<int, 4u>::value_type, std::optional<int>>,
              "a non-pointer element type gets std::optional instead");

} // namespace

int main() {
    acpp::testing::suite suite{"module 09 / wsq_semantics"};

    std::vector<int> values(64);
    for(int i = 0; i < 64; ++i) {
        values[static_cast<std::size_t>(i)] = i;
    }

    // --- the asymmetry ------------------------------------------------------
    {
        queue q;
        for(int i = 0; i < 4; ++i) {
            suite.check(q.try_push(&values[static_cast<std::size_t>(i)]), "push");
        }

        // The owner takes the newest: LIFO. That is the point -- the most
        // recently pushed task is the one still hot in this core's cache.
        suite.check(*q.pop() == 3, "owner pops LIFO");
        suite.check(*q.pop() == 2, "owner pops LIFO");

        // A thief takes the oldest: FIFO. Coarsest-grained, furthest from the
        // owner's working set, least likely to be contended.
        suite.check(*q.steal() == 0, "thief steals FIFO");
        suite.check(*q.steal() == 1, "thief steals FIFO");

        suite.check(q.empty(), "and the queue is drained");
        suite.check(q.pop() == nullptr, "popping empty yields the sentinel");
        suite.check(q.steal() == nullptr, "so does stealing empty");
    }

    // --- capacity -----------------------------------------------------------
    {
        queue q;
        for(int i = 0; i < 16; ++i) {
            suite.check(q.try_push(&values[static_cast<std::size_t>(i)]) || i != 15, "fills to capacity");
        }

        suite.check(q.size() == 16, "16 elements");
        suite.check(!q.try_push(&values[0]), "a full bounded queue refuses");

        // Free-running counters: `top` and `bottom` are never wrapped, only the
        // index into the buffer is. So capacity is decidable without a count,
        // and `top` never repeats a value -- which is why there is no ABA here.
        for(int i = 15; i >= 0; --i) {
            suite.check(*q.pop() == i || i < 0, "drains LIFO");
        }

        suite.check(q.empty(), "empty again");
        suite.check(q.try_push(&values[0]), "and reusable after wrapping the buffer");
    }

    // --- the last element, on one thread ------------------------------------
    //
    // pop() and steal() both CAS `top` when exactly one element remains. Running
    // them in sequence on one thread proves the CAS arbitration works at all,
    // before the stress test asks whether it works under a real race.
    {
        queue q;
        q.try_push(&values[7]);

        suite.check(q.size() == 1, "one element");

        // The owner takes it, which means it wins the CAS against nobody.
        suite.check(*q.pop() == 7, "owner takes the last element");
        suite.check(q.empty(), "queue is empty");

        // And the accounting survived: bottom was decremented, the CAS bumped
        // top, and bottom was restored. If any of those were wrong, size() would
        // be negative or the next push would land in the wrong slot.
        suite.check(q.try_push(&values[9]) && *q.steal() == 9,
                    "the counters are consistent after the last-element path");
    }

    {
        queue q;
        q.try_push(&values[11]);
        suite.check(*q.steal() == 11, "thief takes the last element");
        suite.check(q.pop() == nullptr, "and the owner then finds nothing");
        suite.check(q.size() == 0, "size does not go negative");
    }

    // --- growth -------------------------------------------------------------
    {
        acpp::unbounded_wsq<int *> q{2u}; // start at 4 slots

        suite.check(q.capacity() == 4, "starts small");

        for(int i = 0; i < 64; ++i) {
            q.push(&values[static_cast<std::size_t>(i)]);
        }

        suite.check(q.size() == 64, "grew to hold everything");
        suite.check(q.capacity() >= 64, "capacity followed");
        suite.check(q.resizes() == 4u, "4 doublings: 4 -> 8 -> 16 -> 32 -> 64");

        // Old buffers are retained, not freed: a thief may still hold a pointer
        // into one. Bounded, because each resize doubles, so the retained bytes
        // are bounded by the final capacity.
        suite.check(q.retained_buffers() == q.resizes(), "every replaced buffer is retained");

        for(int i = 63; i >= 0; --i) {
            suite.check(*q.pop() == i || i < 0, "drains LIFO after growth");
        }

        suite.check(q.empty(), "drained");
    }

    return suite.report();
}
