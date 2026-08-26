#ifndef ACPP_HANDLE_HPP
#define ACPP_HANDLE_HPP

// Module 5 -- bit-packed generational handles.
//
// The problem: stable, cheap, copyable handles into a dense array. Raw indices
// dangle once a slot is reused; pointers dangle on reallocation; a shared_ptr
// costs an atomic refcount and a cache miss per dereference.
//
// Generational indices make staleness *detectable* instead of preventing it:
// pack an index and a version into one integer, bump the version on release,
// and a handle holding the old version fails validation. Cost is one compare.
//
// The same shape appears in file descriptors with generation counts, slab
// allocators, GPU resource handles and firmware session handles.

#include "config.hpp"
#include "stl/bit.hpp"
#include "stl/concepts.hpp"
#include "stl/cstddef.hpp"
#include "stl/cstdint.hpp"
#include "stl/limits.hpp"
#include "stl/type_traits.hpp"
#include "stl/vector.hpp"

namespace acpp {

namespace internal {

template<typename>
struct handle_traits;

// An enum whose underlying type has traits inherits them, so
// `enum class widget_id : uint32_t {};` is a first-class handle type with
// nothing else written. The nested `requires requires` is what makes this a
// constraint rather than a hard error for enums whose underlying type has none.
template<typename Type>
    requires requires {
        requires stl::is_enum_v<Type>;
        typename handle_traits<stl::underlying_type_t<Type>>::value_type;
    }
struct handle_traits<Type>: handle_traits<stl::underlying_type_t<Type>> {
    using value_type = Type;
};

// So does a wrapper type that names its own underlying handle type.
template<typename Type>
    requires requires { typename Type::entity_type; }
struct handle_traits<Type>: handle_traits<typename Type::entity_type> {
    using value_type = Type;
};

template<>
struct handle_traits<stl::uint32_t> {
    using value_type = stl::uint32_t;
    using entity_type = stl::uint32_t;
    using version_type = stl::uint16_t;

    // 20 bits of index (1,048,575 live), 12 of version (4,096 reuses).
    static constexpr entity_type index_mask = 0xFFFFF;
    static constexpr entity_type version_mask = 0xFFF;
};

template<>
struct handle_traits<stl::uint64_t> {
    using value_type = stl::uint64_t;
    using entity_type = stl::uint64_t;
    using version_type = stl::uint32_t;

    static constexpr entity_type index_mask = 0xFFFFFFFF;
    static constexpr entity_type version_mask = 0xFFFFFFFF;
};

} // namespace internal

template<typename Type>
concept handle_like = requires { typename internal::handle_traits<Type>::value_type; };

namespace internal {

// conditional_t evaluates both branches' types, so underlying_type_t<uint16_t>
// would be a hard error before the condition ever ran. Lazy, therefore.
template<typename Type>
struct underlying_or_self {
    using type = Type;
};

template<typename Type>
    requires stl::is_enum_v<Type>
struct underlying_or_self<Type> {
    using type = stl::underlying_type_t<Type>;
};

} // namespace internal

/**
 * A bit split stated as bit *counts* rather than as masks.
 *
 * Handy for the "given this budget, choose the split" question: you write the
 * two numbers you reasoned about, and the static_asserts reject a split that
 * does not fit or that leaves one field with no bits.
 */
template<typename Underlying, stl::size_t IndexBits, stl::size_t VersionBits>
struct bit_split {
    static_assert(stl::is_unsigned_v<Underlying>, "signed shifts are a different question; do not");
    static_assert(IndexBits > 0u, "a handle with no index addresses nothing");
    static_assert(VersionBits > 0u, "a handle with no version cannot detect staleness -- use an index");
    static_assert(IndexBits + VersionBits <= stl::numeric_limits<Underlying>::digits,
                  "the split does not fit in the underlying type");

    using value_type = Underlying;
    using entity_type = Underlying;
    using version_type = stl::conditional_t<
        (VersionBits <= 16u), stl::uint16_t,
        stl::conditional_t<(VersionBits <= 32u), stl::uint32_t, stl::uint64_t>>;

    static constexpr entity_type index_mask =
        static_cast<entity_type>((IndexBits == stl::numeric_limits<Underlying>::digits)
                                     ? ~entity_type{}
                                     : ((entity_type{1} << IndexBits) - entity_type{1}));
    static constexpr entity_type version_mask =
        static_cast<entity_type>((entity_type{1} << VersionBits) - entity_type{1});
};

/**
 * The arithmetic, shared by every layout.
 *
 * `length` comes from popcount(index_mask) rather than being a separate
 * constant, so changing the mask is a one-line edit and the shift follows.
 */
template<typename Traits>
class basic_handle_traits {
    static constexpr auto length = stl::popcount(Traits::index_mask);

public:
    using value_type = Traits::value_type;
    using entity_type = Traits::entity_type;
    using version_type = Traits::version_type;

    static constexpr entity_type index_mask = Traits::index_mask;
    static constexpr entity_type version_mask = Traits::version_mask;

    static constexpr stl::size_t index_bits = static_cast<stl::size_t>(stl::popcount(index_mask));
    static constexpr stl::size_t version_bits = static_cast<stl::size_t>(stl::popcount(version_mask));

    [[nodiscard]] static constexpr entity_type to_integral(const value_type value) noexcept {
        return static_cast<entity_type>(value);
    }

    [[nodiscard]] static constexpr entity_type to_index(const value_type value) noexcept {
        // A mask that is not 2^n - 1 would silently corrupt every operation
        // here, so it is checked once, at compile time, where the mask is used.
        static_assert(index_mask != 0u && ((index_mask & (index_mask + 1u)) == 0u), "invalid index mask");
        return to_integral(value) & index_mask;
    }

    [[nodiscard]] static constexpr version_type to_version(const value_type value) noexcept {
        static_assert((version_mask & (version_mask + 1u)) == 0u, "invalid version mask");
        return static_cast<version_type>((to_integral(value) >> length) & version_mask);
    }

    [[nodiscard]] static constexpr value_type construct(const entity_type index,
                                                        const version_type version) noexcept {
        return static_cast<value_type>((index & index_mask)
                                       | (static_cast<entity_type>(version & version_mask) << length));
    }

    /** Index from the first, version from the second. */
    [[nodiscard]] static constexpr value_type combine(const entity_type lhs, const entity_type rhs) noexcept {
        return static_cast<value_type>((lhs & index_mask) | (rhs & (version_mask << length)));
    }

    /**
     * The successor version, skipping the reserved all-ones value.
     *
     *   vers + (vers == version_mask)
     *
     * is a branchless "increment, and jump over version_mask if we landed on
     * it". version_mask is the tombstone's version, so a live handle must never
     * hold it -- which costs exactly one of the 2^VersionBits values.
     */
    [[nodiscard]] static constexpr value_type next(const value_type value) noexcept {
        const auto version = static_cast<version_type>(to_version(value) + 1u);
        return construct(to_integral(value),
                         static_cast<version_type>(version + (version == version_mask)));
    }
};

template<handle_like Type>
struct handle_traits: basic_handle_traits<internal::handle_traits<Type>> {};

/**
 * Null: "no index". Its comparison deliberately looks only at the index part,
 * so a handle that was released and had its version bumped still compares equal
 * to null. Sentinels that compare on the whole word cannot express that.
 */
struct null_t {
    template<handle_like Type>
    [[nodiscard]] constexpr operator Type() const noexcept {
        using traits = handle_traits<Type>;
        return traits::construct(traits::index_mask, static_cast<traits::version_type>(traits::version_mask));
    }

    [[nodiscard]] constexpr bool operator==(const null_t) const noexcept { return true; }

    template<handle_like Type>
    [[nodiscard]] constexpr bool operator==(const Type value) const noexcept {
        using traits = handle_traits<Type>;
        return traits::to_index(value) == traits::to_index(static_cast<Type>(*this));
    }
};

/**
 * Tombstone: "this slot is a hole". Compares only the *version* part, which is
 * what lets one value mark a hole at any index -- Module 6's free list stores a
 * previous hole's index in the index bits of a tombstoned slot.
 */
struct tombstone_t {
    template<handle_like Type>
    [[nodiscard]] constexpr operator Type() const noexcept {
        using traits = handle_traits<Type>;
        return traits::construct(traits::index_mask, static_cast<traits::version_type>(traits::version_mask));
    }

    [[nodiscard]] constexpr bool operator==(const tombstone_t) const noexcept { return true; }

    template<handle_like Type>
    [[nodiscard]] constexpr bool operator==(const Type value) const noexcept {
        using traits = handle_traits<Type>;
        return traits::to_version(value) == traits::to_version(static_cast<Type>(*this));
    }
};

inline constexpr null_t null{};
inline constexpr tombstone_t tombstone{};

template<handle_like Type>
[[nodiscard]] constexpr handle_traits<Type>::entity_type to_index(const Type value) noexcept {
    return handle_traits<Type>::to_index(value);
}

template<handle_like Type>
[[nodiscard]] constexpr handle_traits<Type>::version_type to_version(const Type value) noexcept {
    return handle_traits<Type>::to_version(value);
}

/**
 * What happens when a slot's version runs out.
 *
 * There is no universally right answer, which is why this is a parameter and
 * not a comment. See modules/05-handles/NOTES.md for the reasoning and the
 * arithmetic for choosing a split that keeps you away from it.
 */
enum class exhaustion_policy {
    recycle, //< wrap the version and keep reusing the slot. No leak; staleness
             //  detection becomes best-effort past 2^VersionBits reuses.
    retire,  //< never reuse the slot again. Detection stays exact forever, at
             //  the cost of leaking one index per exhausted slot.
    trap,    //< stop. For systems where neither of the above is acceptable and
             //  reaching this point means a design error upstream.
};

/**
 * Hands out packed handles, recycles released ones through an intrusive free
 * list, and validates.
 *
 * The free list lives in the version array's own slots: a freed slot stores the
 * index of the previously freed slot. Same idea Module 6 uses inside the packed
 * array, at a smaller scale.
 */
template<typename Type, stl::size_t IndexBits, stl::size_t VersionBits,
         exhaustion_policy Policy = exhaustion_policy::recycle>
class handle_allocator {
public:
    using layout_type = bit_split<typename internal::underlying_or_self<Type>::type, IndexBits, VersionBits>;

private:
    struct traits_shim: layout_type {
        using value_type = Type;
    };

public:
    using traits_type = basic_handle_traits<traits_shim>;
    using value_type = Type;
    using entity_type = traits_type::entity_type;
    using version_type = traits_type::version_type;
    using size_type = stl::size_t;

    static constexpr size_type max_slots = static_cast<size_type>(traits_type::index_mask);
    static constexpr version_type max_version = static_cast<version_type>(traits_type::version_mask);
    static constexpr exhaustion_policy policy = Policy;

    // The allocator's own null, not acpp::null. They are different bit patterns
    // whenever the split differs from the type's default one, and asking
    // `acpp::null == h` about a handle from a custom split would be comparing
    // against the wrong mask -- quietly, and only sometimes wrongly.
    [[nodiscard]] static constexpr value_type null_handle() noexcept {
        return traits_type::construct(traits_type::index_mask,
                                      static_cast<version_type>(traits_type::version_mask));
    }

    [[nodiscard]] static constexpr bool is_null(const value_type handle) noexcept {
        return traits_type::to_index(handle) == traits_type::index_mask;
    }

    // Same reasoning: the free acpp::to_index/to_version read the *type's*
    // default split, which is not this allocator's when the bits were given
    // explicitly. Ask the allocator that issued the handle.
    [[nodiscard]] static constexpr entity_type index_of(const value_type handle) noexcept {
        return traits_type::to_index(handle);
    }

    [[nodiscard]] static constexpr version_type version_of(const value_type handle) noexcept {
        return traits_type::to_version(handle);
    }

    [[nodiscard]] value_type allocate() {
        if(head != no_slot) {
            const auto index = head;
            head = links[index];
            links[index] = in_use;
            return traits_type::construct(static_cast<entity_type>(index), versions[index]);
        }

        // No hole to reuse. Two reasons this is a hard stop rather than a
        // grow: an index above the mask aliases an existing handle, and index
        // == index_mask *is* the null handle. So the usable range is
        // [0, index_mask) -- max_slots slots, not max_slots + 1.
        if(versions.size() >= max_slots) {
            return null_handle();
        }

        const auto index = versions.size();
        versions.push_back(version_type{});
        links.push_back(in_use);
        return traits_type::construct(static_cast<entity_type>(index), version_type{});
    }

    bool release(const value_type handle) {
        if(!is_valid(handle)) {
            return false; // double release, or a handle from another allocator
        }

        const auto index = static_cast<size_type>(traits_type::to_index(handle));
        const auto bumped = traits_type::to_version(traits_type::next(traits_type::construct(
            static_cast<entity_type>(index), versions[index])));

        if constexpr(Policy == exhaustion_policy::retire) {
            if(bumped <= versions[index]) {
                // Wrapped. Bump to the reserved version, which no live handle
                // can hold, and never return the slot to the free list.
                versions[index] = max_version;
                ++retired;
                return true;
            }
        } else if constexpr(Policy == exhaustion_policy::trap) {
            if(bumped <= versions[index]) {
                ACPP_TRAP();
            }
        }

        versions[index] = bumped;
        links[index] = head;
        head = index;
        ++recycled;
        return true;
    }

    [[nodiscard]] bool is_valid(const value_type handle) const noexcept {
        const auto index = static_cast<size_type>(traits_type::to_index(handle));

        // Three ways to be invalid, and all three must be checked: out of
        // range, currently free, or the right slot with the wrong generation.
        return index < versions.size()
               && links[index] == in_use
               && versions[index] == traits_type::to_version(handle);
    }

    [[nodiscard]] size_type size() const noexcept { return versions.size(); }
    [[nodiscard]] size_type live() const noexcept { return versions.size() - free_slots() - retired; }
    [[nodiscard]] size_type recycles() const noexcept { return recycled; }
    [[nodiscard]] size_type retirements() const noexcept { return retired; }

    [[nodiscard]] size_type free_slots() const noexcept {
        size_type count = 0u;
        for(auto cursor = head; cursor != no_slot; cursor = links[cursor]) {
            ++count;
        }
        return count;
    }

private:
    static constexpr size_type no_slot = static_cast<size_type>(-1);
    static constexpr size_type in_use = static_cast<size_type>(-2);

    stl::vector<version_type> versions;
    stl::vector<size_type> links;
    size_type head{no_slot};
    size_type recycled{};
    size_type retired{};
};

} // namespace acpp

#endif // ACPP_HANDLE_HPP
