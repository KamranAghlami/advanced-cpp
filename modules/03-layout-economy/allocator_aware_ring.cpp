// Module 3, exercise 2 -- an existing container made allocator-aware without
// paying for the stateless case.

#include <cstddef>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

#include <acpp/ring_buffer.hpp>
#include <acpp/testing.hpp>

namespace {

// A stateful allocator, to prove the compression is compression and not a
// silently dropped member.
template<typename Type>
struct counting_allocator {
    using value_type = Type;

    struct budget {
        std::size_t allocations{};
        std::size_t live_bytes{};
    };

    budget *ledger;

    explicit counting_allocator(budget *target) noexcept
        : ledger{target} {}

    template<typename Other>
    counting_allocator(const counting_allocator<Other> &other) noexcept
        : ledger{other.ledger} {}

    [[nodiscard]] Type *allocate(const std::size_t count) {
        ++ledger->allocations;
        ledger->live_bytes += count * sizeof(Type);
        return static_cast<Type *>(::operator new(count * sizeof(Type)));
    }

    void deallocate(Type *pointer, const std::size_t count) noexcept {
        ledger->live_bytes -= count * sizeof(Type);
        ::operator delete(pointer);
    }

    bool operator==(const counting_allocator &) const noexcept = default;
};

// The reference point: the same container with the allocator hard-coded away.
struct hand_written_ring {
    int *data;
    std::size_t mask;
    std::size_t head;
    std::size_t tail;
};

using default_ring = acpp::ring_buffer<int>;
using counted_ring = acpp::ring_buffer<int, counting_allocator<int>>;

// The claim: allocator-awareness is free when the allocator is stateless.
static_assert(std::is_empty_v<std::allocator<int>>);
static_assert(sizeof(default_ring) == sizeof(hand_written_ring),
              "a stateless allocator must cost zero bytes");

// And when it is not stateless, it costs exactly itself -- no padding surprise.
static_assert(sizeof(counted_ring) == sizeof(hand_written_ring) + sizeof(void *));

} // namespace

int main() {
    acpp::testing::suite suite{"module 03 / allocator_aware_ring"};

    suite.note("hand_written=%zu  ring_buffer<int>=%zu  ring_buffer<int, counting>=%zu",
               sizeof(hand_written_ring), sizeof(default_ring), sizeof(counted_ring));

    suite.check(sizeof(default_ring) == sizeof(hand_written_ring), "stateless allocator adds nothing");
    suite.check(sizeof(counted_ring) == sizeof(hand_written_ring) + sizeof(void *),
                "stateful allocator adds exactly itself");

    // Behaviour: capacity rounds up to a power of two so wrapping is a mask.
    default_ring ring{5u};
    suite.check(ring.capacity() == 8u, "capacity rounds up to a power of two");
    suite.check(ring.empty() && ring.size() == 0u, "starts empty");

    for(int i = 0; i < 8; ++i) {
        (void)ring.push(i);
    }

    suite.check(ring.full() && ring.size() == 8u, "fills to capacity");
    suite.check(!ring.push(99), "a full ring refuses");

    // Free-running counters, so full and empty are distinguishable without a
    // wasted slot or a separate count. Drain and refill across the wrap point.
    for(int i = 0; i < 4; ++i) {
        ring.pop();
    }

    for(int i = 8; i < 12; ++i) {
        (void)ring.push(i);
    }

    bool ordered = true;
    for(int expected = 4; expected < 12; ++expected) {
        ordered = ordered && (ring.front() == expected);
        ring.pop();
    }

    suite.check(ordered, "FIFO order survives wrapping");
    suite.check(ring.empty(), "drains empty");

    // The allocator is genuinely used, and genuinely returns its memory.
    counting_allocator<int>::budget ledger;
    {
        counted_ring counted{16u, counting_allocator<int>{&ledger}};
        (void)counted.push(1);
        suite.check(ledger.allocations == 1u, "one allocation for the whole ring");
        suite.check(ledger.live_bytes == 16u * sizeof(int), "allocated the rounded capacity");
    }

    suite.check(ledger.live_bytes == 0u, "everything was returned");

    // Non-trivial elements must be constructed and destroyed, not memcpy'd.
    acpp::ring_buffer<std::string> strings{4u};
    (void)strings.emplace(3u, 'x');
    suite.check(strings.front() == "xxx", "emplace forwards to the element constructor");
    strings.pop();
    suite.check(strings.empty(), "destroys on pop");

    return suite.report();
}
