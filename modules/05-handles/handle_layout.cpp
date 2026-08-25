// Module 5 -- the packed layout itself: masks, the shift derived from popcount,
// the two sentinels, and the trait inheritance that makes a user's enum a
// first-class handle type.

#include <cstdint>
#include <type_traits>

#include <acpp/handle.hpp>
#include <acpp/testing.hpp>

namespace {

using acpp::handle_traits;
using acpp::null;
using acpp::tombstone;

// A user's own handle type, with nothing written but the enum. The traits come
// from uint32_t through the constrained specialization in handle.hpp.
enum class widget_id : std::uint32_t {};

// A wrapper type gets there the other way, by naming its underlying type.
struct session_id {
    using entity_type = std::uint64_t;
    entity_type value{};
    constexpr explicit operator entity_type() const noexcept { return value; }
};

using widget_traits = handle_traits<widget_id>;
using u32_traits = handle_traits<std::uint32_t>;
using u64_traits = handle_traits<std::uint64_t>;

// --- the layout -------------------------------------------------------------

static_assert(u32_traits::index_mask == 0xFFFFF);   // 20 bits, 1,048,575 live
static_assert(u32_traits::version_mask == 0xFFF);   // 12 bits, 4,096 reuses
static_assert(u32_traits::index_bits == 20u);
static_assert(u32_traits::version_bits == 12u);
static_assert(u32_traits::index_bits + u32_traits::version_bits == 32u, "the split must not waste bits");

static_assert(u64_traits::index_bits == 32u);
static_assert(u64_traits::version_bits == 32u);

// The enum inherits the split, and keeps its own value type.
static_assert(widget_traits::index_mask == u32_traits::index_mask);
static_assert(std::is_same_v<widget_traits::value_type, widget_id>);
static_assert(acpp::handle_like<widget_id>);
static_assert(acpp::handle_like<session_id>);
static_assert(!acpp::handle_like<float>, "a handle type must have traits, not merely be an integer");

// The version type is chosen to hold the version, not to match the handle.
static_assert(std::is_same_v<u32_traits::version_type, std::uint16_t>);
static_assert(std::is_same_v<u64_traits::version_type, std::uint32_t>);

// --- the arithmetic ---------------------------------------------------------

constexpr auto sample = widget_traits::construct(1234u, 7u);

static_assert(widget_traits::to_index(sample) == 1234u);
static_assert(widget_traits::to_version(sample) == 7u);
static_assert(widget_traits::to_integral(sample) == (1234u | (7u << 20u)));

// combine takes the index from the first and the version from the second --
// the operation a container needs when it moves a payload between slots.
static_assert(widget_traits::to_index(widget_traits::combine(
                  widget_traits::to_integral(widget_traits::construct(5u, 1u)),
                  widget_traits::to_integral(widget_traits::construct(9u, 2u)))) == 5u);
static_assert(widget_traits::to_version(widget_traits::combine(
                  widget_traits::to_integral(widget_traits::construct(5u, 1u)),
                  widget_traits::to_integral(widget_traits::construct(9u, 2u)))) == 2u);

// next() bumps the version and leaves the index alone.
static_assert(widget_traits::to_index(widget_traits::next(sample)) == 1234u);
static_assert(widget_traits::to_version(widget_traits::next(sample)) == 8u);

// --- the sentinels ----------------------------------------------------------
//
// null and tombstone have the same bit pattern and different comparisons, and
// that asymmetry is the design. null asks "is this any slot at all?" and looks
// only at the index; tombstone asks "is this slot a hole?" and looks only at the
// version. Module 6's free list depends on the second: a tombstoned packed slot
// stores a *different* index in its index bits while still comparing equal to
// tombstone.

static_assert(null == static_cast<widget_id>(null));
static_assert(tombstone == static_cast<widget_id>(tombstone));

// A released handle keeps its index, so it still compares equal to null only if
// the index really is the null index. Bumping the version does not change that.
static_assert(!(null == widget_traits::construct(3u, 0u)));
static_assert(null == widget_traits::construct(widget_traits::index_mask, 0u),
              "null compares the index part only");
static_assert(tombstone == widget_traits::construct(3u, widget_traits::version_mask),
              "tombstone compares the version part only -- any index, marked dead");

// Which is exactly the encoding Module 6 will store in a hole.
static_assert(widget_traits::to_index(
                  widget_traits::combine(41u, widget_traits::to_integral(static_cast<widget_id>(tombstone)))) == 41u);
static_assert(tombstone == widget_traits::combine(41u, widget_traits::to_integral(static_cast<widget_id>(tombstone))));

// --- a custom split ---------------------------------------------------------

using tiny = acpp::basic_handle_traits<acpp::bit_split<std::uint16_t, 10u, 6u>>;

static_assert(tiny::index_mask == 0x3FF);
static_assert(tiny::version_mask == 0x3F);
static_assert(tiny::to_index(tiny::construct(1000u, 60u)) == 1000u);
static_assert(tiny::to_version(tiny::construct(1000u, 60u)) == 60u);

} // namespace

int main() {
    acpp::testing::suite suite{"module 05 / handle_layout"};

    suite.note("uint32 split: %zu index bits (%u live) + %zu version bits (%u reuses)",
               u32_traits::index_bits, u32_traits::index_mask,
               u32_traits::version_bits, u32_traits::version_mask);

    suite.check(widget_traits::to_index(sample) == 1234u, "index round-trips");
    suite.check(widget_traits::to_version(sample) == 7u, "version round-trips");
    suite.check(sizeof(widget_id) == sizeof(std::uint32_t), "a handle is one word and nothing else");

    // Every index/version pair in a small split must round-trip. Cheap enough to
    // check exhaustively, which beats checking three of them.
    bool all_round_trip = true;
    for(std::uint32_t index = 0u; index <= tiny::index_mask; ++index) {
        for(std::uint32_t version = 0u; version <= tiny::version_mask; ++version) {
            const auto packed = tiny::construct(static_cast<std::uint16_t>(index),
                                                static_cast<std::uint16_t>(version));
            all_round_trip = all_round_trip
                             && tiny::to_index(packed) == index
                             && tiny::to_version(packed) == version;
        }
    }

    suite.check(all_round_trip, "all 65,536 index/version pairs round-trip in a 10/6 split");

    // next() must skip the reserved version -- a live handle holding it would
    // compare equal to tombstone.
    auto handle = widget_traits::construct(1u, static_cast<std::uint16_t>(widget_traits::version_mask - 1u));
    handle = widget_traits::next(handle);
    suite.check(widget_traits::to_version(handle) != widget_traits::version_mask,
                "next() steps over the reserved tombstone version");
    suite.check(!(tombstone == handle), "so a live handle never looks like a hole");

    return suite.report();
}
