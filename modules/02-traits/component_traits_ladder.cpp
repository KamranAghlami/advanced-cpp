// Module 2 -- component_traits, and the checkpoint question answered in code.
//
// Checkpoint: why is `in_place_delete` defaulting to the inverse of movability a
// *correctness* decision rather than a performance one?
//
// Because swap-and-pop is not an available implementation for a non-movable
// type. Erasing under that policy means "move the last element into the hole",
// and a type without a move constructor and move assignment cannot be moved
// there. The default is not a guess about what the user probably wants; it is
// the only policy that compiles. Performance enters one rung up, where a movable
// type that nonetheless needs stable addresses opts in.

#include <memory>
#include <string>
#include <type_traits>

#include <acpp/component.hpp>
#include <acpp/testing.hpp>

namespace {

using entity_type = std::uint32_t;

template<typename Type>
using traits = acpp::component_traits<Type, entity_type>;

struct position {
    float x, y;
};

// Empty: page_size 0, so no payload array exists at all (Module 3 and 7).
struct stationary {};

// Movable, so swap-and-pop is available and inferred.
static_assert(!traits<position>::in_place_delete);
static_assert(traits<position>::page_size == ACPP_PACKED_PAGE);
static_assert(traits<stationary>::page_size == 0u);

// --- level 1: inference, and what it is really saying ----------------------

// A mutex is neither move constructible nor move assignable, so swap-and-pop
// cannot be implemented for it. in_place_delete is inferred true not as a
// preference but as the only option.
struct has_mutex {
    std::string name;
    std::mutex guard;
};

static_assert(!std::is_move_constructible_v<has_mutex>);
static_assert(traits<has_mutex>::in_place_delete, "non-movable types must not be swap-and-popped");

// The converse, spelled out: for every type where the inference says false, the
// operations swap-and-pop needs are actually available.
template<typename Type>
constexpr bool inference_is_sound =
    traits<Type>::in_place_delete || (std::is_move_constructible_v<Type> && std::is_move_assignable_v<Type>);

static_assert(inference_is_sound<position>);
static_assert(inference_is_sound<std::string>);
static_assert(inference_is_sound<std::unique_ptr<int>>);
static_assert(inference_is_sound<has_mutex>);

// --- level 2: the inline opt-in --------------------------------------------

// Perfectly movable. The reason it wants in-place deletion is external: other
// structures hold pointers into the pool and must keep them valid. No amount of
// inspecting the type could have discovered that, which is exactly why level 2
// exists and why level 1 must not try to guess it.
struct pinned {
    int value;
    static constexpr bool in_place_delete = true;
};

static_assert(std::is_move_constructible_v<pinned> && std::is_move_assignable_v<pinned>);
static_assert(traits<pinned>::in_place_delete, "the member must beat the inference");

// Same rung, other trait: a rarely-used component should not reserve a
// 1024-slot page.
struct rare {
    double payload;
    static constexpr std::size_t page_size = 16u;
};

static_assert(traits<rare>::page_size == 16u);

// --- level 3: the full override --------------------------------------------

struct third_party {
    int value;
};

} // namespace

namespace acpp {

// third_party is not ours to edit, and its inferred answers are both wrong for
// how we use it. This is the only rung that can say so.
template<>
struct component_traits<third_party, entity_type> {
    using element_type = third_party;
    using entity_type = ::entity_type;

    static constexpr bool in_place_delete = true;
    static constexpr std::size_t page_size = 4u;
};

} // namespace acpp

namespace {

static_assert(traits<third_party>::in_place_delete);
static_assert(traits<third_party>::page_size == 4u);

// The ladder must be ordered, and the guard on component_traits must reject the
// argument nobody meant to pass.
template<typename Type>
concept has_traits = requires { typename acpp::component_traits<Type, entity_type>; };

static_assert(has_traits<position>);
static_assert(!has_traits<const position>, "cvref-qualified types are not components");
static_assert(!has_traits<position &>);

} // namespace

int main() {
    acpp::testing::suite suite{"module 02 / component_traits_ladder"};

    suite.check(!traits<position>::in_place_delete, "level 1: movable infers swap-and-pop");
    suite.check(traits<has_mutex>::in_place_delete, "level 1: non-movable infers in-place");
    suite.check(traits<pinned>::in_place_delete, "level 2: member opts in");
    suite.check(traits<rare>::page_size == 16u, "level 2: member sets the page size");
    suite.check(traits<third_party>::page_size == 4u, "level 3: specialization overrides both");

    suite.check(traits<stationary>::page_size == 0u, "empty type gets no payload pages");
    suite.check(traits<position>::page_size == ACPP_PACKED_PAGE, "non-empty type gets the default page");

    // The multiplication-by-a-bool idiom: one expression, no branch, and the
    // empty case falls out as zero rather than as a special case downstream.
    suite.note("page_size(empty)=%zu page_size(position)=%zu",
               traits<stationary>::page_size, traits<position>::page_size);

    return suite.report();
}
