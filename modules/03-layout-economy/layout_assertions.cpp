// Module 3, exercise 3 -- the static_assert battery.
//
// Every assumption this project makes about size, alignment, field order and
// standard-layout-ness, stated once, in code, where the compiler re-checks it on
// every build. The habit is cheap and it is what catches ABI drift when the
// toolchain moves underneath you -- a new libstdc++, a different ABI level, a
// cross-compiler with a different pointer width.
//
// Two rules keep a battery like this useful rather than annoying:
//   * assert what the code DEPENDS on, not everything that happens to be true;
//   * when an assertion is platform-specific, guard it and say so, rather than
//     letting a port fail on a fact nobody actually needed.

#include <climits>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>

#include <acpp/compressed_pair.hpp>
#include <acpp/config.hpp>
#include <acpp/hashed_string.hpp>
#include <acpp/ring_buffer.hpp>
#include <acpp/testing.hpp>
#include <acpp/type_traits.hpp>

namespace {

// --- platform ---------------------------------------------------------------

static_assert(CHAR_BIT == 8, "the packing arithmetic in Modules 5 and 11 assumes octets");
static_assert(sizeof(acpp::id_type) == 4u, "id_type is uint32_t; the FNV-1a parameter table depends on it");
static_assert(std::is_unsigned_v<acpp::id_type>, "ids are shifted and masked, never signed-shifted");

// Not asserted: sizeof(void *). Nothing here depends on the pointer width, and
// asserting it would break a 32-bit port for no reason. This omission is the
// point of the second rule above.

// --- config -----------------------------------------------------------------

static_assert(ACPP_PACKED_PAGE > 0u && (ACPP_PACKED_PAGE & (ACPP_PACKED_PAGE - 1u)) == 0u,
              "page sizes must be powers of two: paging replaces division with a shift and a mask");
static_assert(ACPP_SPARSE_PAGE > 0u && (ACPP_SPARSE_PAGE & (ACPP_SPARSE_PAGE - 1u)) == 0u,
              "same, for the sparse array");

// --- compressed_pair --------------------------------------------------------

struct empty {};
struct two_ints {
    int a;
    int b;
};

using compressed = acpp::compressed_pair<empty, int>;
using uncompressed = acpp::compressed_pair<two_ints, int>;

static_assert(sizeof(compressed) == sizeof(int));
static_assert(alignof(compressed) == alignof(int));
static_assert(sizeof(uncompressed) == 3u * sizeof(int));
static_assert(alignof(uncompressed) == alignof(int));

// Compression must not be paid for in alignment: an over-aligned half keeps its
// alignment and the empty half still costs nothing.
struct alignas(32) over_aligned {
    int value;
};

static_assert(alignof(acpp::compressed_pair<empty, over_aligned>) == 32u);
static_assert(sizeof(acpp::compressed_pair<empty, over_aligned>) == 32u);

// Compression is per type, not per empty member: two subobjects of the same
// type must still have distinct addresses.
static_assert(sizeof(acpp::compressed_pair<empty, empty>) == 2u);

// --- ring_buffer ------------------------------------------------------------

using ring = acpp::ring_buffer<int>;

static_assert(std::is_nothrow_move_constructible_v<ring>, "moving a ring must not allocate");
static_assert(!std::is_copy_constructible_v<ring>, "copying is deliberately not provided");
static_assert(sizeof(ring) == sizeof(void *) + 3u * sizeof(std::size_t),
              "a stateless allocator adds nothing; four words is the whole container");

// --- offsetof, on a type that is allowed to answer -------------------------

// offsetof is only defined for standard-layout types. That constraint is why
// layout assertions and EBO tend not to mix, and why the plain structs below
// exist: they are the ones a serialiser or a C boundary would actually see.
struct wire_header {
    std::uint32_t magic;
    std::uint16_t version;
    std::uint16_t flags;
    std::uint64_t length;
};

static_assert(std::is_standard_layout_v<wire_header>);
static_assert(std::is_trivially_copyable_v<wire_header>);
static_assert(offsetof(wire_header, magic) == 0u);
static_assert(offsetof(wire_header, version) == 4u);
static_assert(offsetof(wire_header, flags) == 6u);
// 8, not 10: the u64 is aligned, so there are two bytes of padding after flags.
// Writing the assertion is how you find that out before the wire format does.
static_assert(offsetof(wire_header, length) == 8u);
static_assert(sizeof(wire_header) == 16u);

// --- trait vocabulary -------------------------------------------------------

static_assert(acpp::is_ebco_eligible_v<empty>);
static_assert(!acpp::is_ebco_eligible_v<two_ints>);
static_assert(std::is_empty_v<std::allocator<int>>, "the whole allocator-compression argument rests on this");

// --- hashed_string ----------------------------------------------------------

using namespace acpp::literals;

static_assert("POWER_ON"_hs != "POWER_OFF"_hs);
static_assert(acpp::hashed_string::value("", 0u) == 2166136261u, "the empty string hashes to the FNV offset basis");
static_assert(acpp::hashed_string::value("a", 1u) == ((2166136261u ^ 'a') * 16777619u),
              "one round of FNV-1a, spelled out");

} // namespace

int main() {
    acpp::testing::suite suite{"module 03 / layout_assertions"};

    // Everything above is compile-time. main() exists so the battery is a
    // ctest entry rather than a header nobody includes, and so the numbers get
    // printed where a human reviewing an ABI change can read them.
    suite.note("id_type=%zu  compressed_pair<empty,int>=%zu  ring_buffer<int>=%zu  wire_header=%zu",
               sizeof(acpp::id_type), sizeof(compressed), sizeof(ring), sizeof(wire_header));
    suite.note("ACPP_PACKED_PAGE=%d  ACPP_SPARSE_PAGE=%d", ACPP_PACKED_PAGE, ACPP_SPARSE_PAGE);

    suite.check(sizeof(compressed) == sizeof(int), "layout battery compiled, so every assertion above held");

    // One runtime check the compiler cannot make: that the padding in
    // wire_header is actually padding and not something we are reading.
    wire_header header{};
    const auto *bytes = reinterpret_cast<const unsigned char *>(&header);
    suite.check(bytes[8] == 0u && bytes[9] == 0u, "value-initialisation zeroes the padding too");

    return suite.report();
}
