// Module 5, exercise 1 -- handle_allocator<Type, IndexBits, VersionBits>.
//
// allocate() hands out a packed handle, release() bumps the version and pushes
// the index onto a free list, is_valid() checks. The split is a template
// parameter, and bit_split's static_asserts reject one that does not fit.

#include <cstdint>

#include <acpp/handle.hpp>
#include <acpp/testing.hpp>

namespace {

enum class widget_id : std::uint32_t {};

using allocator = acpp::handle_allocator<widget_id, 20u, 12u>;

static_assert(allocator::max_slots == 0xFFFFF);
static_assert(allocator::max_version == 0xFFF);

// A split that does not fit its underlying type is a compile error, not a
// runtime surprise. modules/05-handles/oversized_split_fails.cpp is that code.
static_assert(sizeof(acpp::bit_split<std::uint32_t, 20u, 12u>::entity_type) == 4u);

} // namespace

int main() {
    acpp::testing::suite suite{"module 05 / handle_allocator"};

    allocator handles;

    const auto first = handles.allocate();
    const auto second = handles.allocate();

    suite.check(handles.is_valid(first) && handles.is_valid(second), "fresh handles validate");
    suite.check(first != second, "and are distinct");
    suite.check(acpp::to_index(first) == 0u && acpp::to_index(second) == 1u, "indices are dense");
    suite.check(acpp::to_version(first) == 0u, "versions start at zero");
    suite.check(handles.live() == 2u, "two live");

    // The point of the whole design.
    const auto stale = first;
    suite.check(handles.release(first), "release succeeds");
    suite.check(!handles.is_valid(stale), "the stale copy stops validating immediately");
    suite.check(!handles.release(stale), "and a double release is refused");

    // The freed slot is reused, with a bumped version.
    const auto reused = handles.allocate();
    suite.check(acpp::to_index(reused) == acpp::to_index(stale), "the free list reused the slot");
    suite.check(acpp::to_version(reused) == acpp::to_version(stale) + 1u, "with the version bumped");
    suite.check(!handles.is_valid(stale), "so the old handle still fails");
    suite.check(handles.is_valid(reused), "and the new one passes");
    suite.check(handles.size() == 2u, "no new slot was needed");

    // LIFO: the most recently freed slot comes back first. Not a requirement of
    // the design, but it is what an intrusive free list gives you for free, and
    // it is the cache-friendly order.
    const auto a = handles.allocate();
    const auto b = handles.allocate();
    (void)handles.release(a);
    (void)handles.release(b);
    suite.check(acpp::to_index(handles.allocate()) == acpp::to_index(b), "free list is LIFO");
    suite.check(acpp::to_index(handles.allocate()) == acpp::to_index(a), "free list is LIFO");

    // A handle that was never issued must not validate, whatever its bits say.
    using traits = allocator::traits_type;
    suite.check(!handles.is_valid(traits::construct(999999u, 0u)), "an out-of-range index is invalid");
    suite.check(!handles.is_valid(traits::construct(0u, 900u)), "an in-range index with a wrong version is invalid");

    // Running out of index space must be visible, not silently aliasing.
    using tiny_allocator = acpp::handle_allocator<std::uint16_t, 4u, 4u>;
    tiny_allocator tiny;
    std::size_t issued = 0u;
    while(!tiny_allocator::is_null(tiny.allocate())) {
        ++issued;
        if(issued > 64u) {
            break;
        }
    }

    // max_slots, not max_slots + 1: index_mask itself is the null handle, so the
    // usable range is [0, index_mask). Issuing that last index would have
    // produced a handle indistinguishable from null -- which is exactly what
    // this check was written to catch, and did.
    suite.note("a 4-bit index yields %zu usable slots (mask is %u)", issued, tiny_allocator::max_slots);
    suite.check(issued == tiny_allocator::max_slots, "exhaustion returns null rather than aliasing");
    suite.check(tiny_allocator::is_null(tiny.allocate()), "and keeps returning null");

    return suite.report();
}
