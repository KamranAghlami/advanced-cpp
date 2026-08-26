#ifndef ACPP_STORAGE_HPP
#define ACPP_STORAGE_HPP

// Module 7 -- typed storage on top of the sparse set.
//
// The payload is paged, like the sparse array, but for a completely different
// reason and with a different trade-off:
//
//   paged SPARSE   because it is indexed by entity id, which is sparse. Flat
//                  would waste memory proportional to the largest id. Bounds
//                  waste to one page. Trade: one indirection per lookup.
//
//   paged PAYLOAD  because pages are never reallocated, so an element's address
//                  is stable for its whole lifetime. A flat std::vector would
//                  invalidate every pointer on growth. Trade: one indirection
//                  per access, plus page-aware iteration.
//
// Same mechanism, opposite motivations. Getting that distinction straight is the
// module's checkpoint.

#include <cstdlib>

#include "bit.hpp"
#include "component.hpp"
#include "config.hpp"
#include "sparse_set.hpp"
#include "stl/cstddef.hpp"
#include "stl/iterator.hpp"
#include "stl/memory.hpp"
#include "stl/tuple.hpp"
#include "stl/type_traits.hpp"
#include "stl/utility.hpp"
#include "stl/vector.hpp"

namespace acpp {

namespace internal {

/**
 * Iterator over a paged payload. Runs **backward**, like the sparse set's.
 *
 * Dereference computes `pos / Page` and `pos % Page` on every access. That is
 * fine, and worth being precise about: `Page` is a compile-time power of two, so
 * both are a shift and a mask. The codegen probe in
 * modules/07-storage-and-views proves it rather than assuming it -- this is a
 * good calibration for when "a division per element" is and is not a cost.
 */
template<typename Container, stl::size_t Page>
class storage_iterator final {
    template<typename, stl::size_t>
    friend class storage_iterator;

    using container_type = stl::remove_const_t<Container>;
    using element_type = stl::remove_pointer_t<typename container_type::value_type>;

public:
    using value_type = stl::remove_const_t<element_type>;
    using pointer = stl::conditional_t<stl::is_const_v<Container>, const value_type *, value_type *>;
    using reference = stl::conditional_t<stl::is_const_v<Container>, const value_type &, value_type &>;
    using difference_type = stl::ptrdiff_t;
    using iterator_category = stl::random_access_iterator_tag;

    constexpr storage_iterator() noexcept = default;

    constexpr storage_iterator(Container *ref, const difference_type idx) noexcept
        : payload{ref}, offset{idx} {}

    template<typename Other>
        requires(stl::is_const_v<Container> && stl::is_same_v<Other, container_type>)
    constexpr storage_iterator(const storage_iterator<Other, Page> &other) noexcept
        : storage_iterator{other.payload, other.offset} {}

    constexpr storage_iterator &operator++() noexcept { return --offset, *this; }
    constexpr storage_iterator operator++(int) noexcept { const auto copy = *this; return ++*this, copy; }
    constexpr storage_iterator &operator--() noexcept { return ++offset, *this; }
    constexpr storage_iterator operator--(int) noexcept { const auto copy = *this; return --*this, copy; }

    constexpr storage_iterator &operator+=(const difference_type value) noexcept { offset -= value; return *this; }
    constexpr storage_iterator &operator-=(const difference_type value) noexcept { return *this += -value; }

    [[nodiscard]] constexpr storage_iterator operator+(const difference_type value) const noexcept {
        auto copy = *this;
        return copy += value;
    }

    [[nodiscard]] constexpr storage_iterator operator-(const difference_type value) const noexcept {
        return *this + -value;
    }

    [[nodiscard]] constexpr difference_type operator-(const storage_iterator &other) const noexcept {
        return other.offset - offset;
    }

    [[nodiscard]] constexpr reference operator[](const difference_type value) const noexcept {
        const auto pos = static_cast<stl::size_t>(index() - value);
        return (*payload)[pos / Page][fast_mod(pos, Page)];
    }

    [[nodiscard]] constexpr reference operator*() const noexcept { return (*this)[0]; }
    [[nodiscard]] constexpr pointer operator->() const noexcept { return &operator*(); }

    [[nodiscard]] constexpr stl::size_t index() const noexcept {
        return static_cast<stl::size_t>(offset - 1);
    }

    [[nodiscard]] constexpr bool operator==(const storage_iterator &other) const noexcept {
        return offset == other.offset;
    }

    [[nodiscard]] constexpr auto operator<=>(const storage_iterator &other) const noexcept {
        return other.offset <=> offset;
    }

private:
    Container *payload{};
    difference_type offset{};
};

/**
 * Zips a base iterator with N others and yields a tuple on dereference.
 *
 * This is what makes `for(auto [entity, pos, vel] : view.each())` work, and the
 * empty-type case falls out of it for free: `get_as_tuple` returns an empty
 * tuple for a tag component and `tuple_cat` simply skips it. No special case
 * anywhere downstream.
 */
template<typename It, typename... Other>
class extended_iterator final {
public:
    using iterator_type = It;
    using difference_type = stl::ptrdiff_t;
    // `Other` is a pointer to a storage, so the call is through `->`. Spelling
    // the return type this way -- rather than naming a tuple of references --
    // is what lets an empty storage contribute an empty tuple and vanish.
    using value_type = decltype(stl::tuple_cat(stl::make_tuple(*stl::declval<It>()),
                                               stl::declval<Other>()->get_as_tuple(*stl::declval<It>())...));
    using reference = value_type;
    using iterator_category = stl::input_iterator_tag;

    constexpr extended_iterator() = default;

    constexpr extended_iterator(iterator_type base, Other... other) noexcept
        : it{base}, pools{other...} {}

    constexpr extended_iterator &operator++() noexcept { return ++it, *this; }
    constexpr extended_iterator operator++(int) noexcept { const auto copy = *this; return ++*this, copy; }

    [[nodiscard]] constexpr reference operator*() const noexcept {
        // An immediately-invoked generic lambda over an index_sequence: the only
        // way to expand a tuple of pools and a single entity into one flat
        // tuple without naming an intermediate type.
        return [&]<stl::size_t... Index>(stl::index_sequence<Index...>) {
            return stl::tuple_cat(stl::make_tuple(*it), stl::get<Index>(pools)->get_as_tuple(*it)...);
        }(stl::index_sequence_for<Other...>{});
    }

    [[nodiscard]] constexpr bool operator==(const extended_iterator &other) const noexcept {
        return it == other.it;
    }

    [[nodiscard]] constexpr iterator_type base() const noexcept { return it; }

private:
    It it{};
    stl::tuple<Other...> pools{};
};

/**
 * Zips the entity iterator with the payload iterator, advancing both.
 *
 * The distinction from extended_iterator is the whole reason both exist. A
 * *view* leads with one pool and must look the entity up in the others, so a
 * lookup per element is unavoidable there. A *storage* iterating itself has the
 * payload sitting at the same packed position, so a lookup would be pure waste
 * -- and it is easy to write it that way by accident, because the pool-lookup
 * version compiles and produces the right answer.
 */
template<typename It, typename... Payload>
class extended_storage_iterator final {
public:
    using difference_type = stl::ptrdiff_t;
    using value_type = decltype(stl::tuple_cat(stl::make_tuple(*stl::declval<It>()),
                                               stl::forward_as_tuple(*stl::declval<Payload>())...));
    using reference = value_type;
    using iterator_category = stl::input_iterator_tag;

    constexpr extended_storage_iterator() = default;

    constexpr extended_storage_iterator(It base, Payload... other) noexcept
        : it{base}, payload{other...} {}

    constexpr extended_storage_iterator &operator++() noexcept {
        ++it;
        stl::apply([](auto &...curr) { (++curr, ...); }, payload);
        return *this;
    }

    constexpr extended_storage_iterator operator++(int) noexcept {
        const auto copy = *this;
        return ++*this, copy;
    }

    [[nodiscard]] constexpr reference operator*() const noexcept {
        return stl::apply(
            [this](auto &...curr) { return stl::tuple_cat(stl::make_tuple(*it), stl::forward_as_tuple(*curr)...); },
            payload);
    }

    [[nodiscard]] constexpr bool operator==(const extended_storage_iterator &other) const noexcept {
        return it == other.it;
    }

private:
    It it{};
    stl::tuple<Payload...> payload{};
};

/** A range over a pair of iterators. */
template<typename It>
struct iterable_adaptor final {
    using iterator = It;

    constexpr iterable_adaptor() = default;
    constexpr iterable_adaptor(iterator from, iterator to) noexcept
        : first{from}, last{to} {}

    [[nodiscard]] constexpr iterator begin() const noexcept { return first; }
    [[nodiscard]] constexpr iterator end() const noexcept { return last; }

private:
    iterator first{};
    iterator last{};
};

} // namespace internal

/**
 * Typed storage: a sparse set plus a paged payload kept in lockstep with it.
 *
 * The parallel array is kept in sync entirely through the base's private-virtual
 * seam, so the base still knows nothing about Type.
 */
template<typename Type, handle_like Entity = stl::uint32_t,
         typename Allocator = stl::allocator<Type>>
class basic_storage: public basic_sparse_set<Entity, typename stl::allocator_traits<Allocator>::template rebind_alloc<Entity>> {
    using alloc_traits = stl::allocator_traits<Allocator>;
    using traits_type = component_traits<Type, Entity>;
    using base_type = basic_sparse_set<Entity, typename alloc_traits::template rebind_alloc<Entity>>;
    using container_type = stl::vector<typename alloc_traits::pointer,
                                       typename alloc_traits::template rebind_alloc<typename alloc_traits::pointer>>;

public:
    using element_type = Type;
    using value_type = Type;
    using entity_type = Entity;
    using allocator_type = Allocator;
    using size_type = stl::size_t;
    using iterator = internal::storage_iterator<container_type, traits_type::page_size>;
    using const_iterator = internal::storage_iterator<const container_type, traits_type::page_size>;

    static constexpr size_type page_size = traits_type::page_size;
    static_assert(is_valid_page_size(page_size), "payload paging needs a power-of-two page size");

    /** The policy is *derived*, not chosen: a non-movable type cannot be
     *  swap-and-popped at all. Module 2's correctness argument, cashed in. */
    static constexpr deletion_policy storage_policy =
        traits_type::in_place_delete ? deletion_policy::in_place : deletion_policy::swap_and_pop;

    explicit basic_storage(const allocator_type &allocator = {})
        : base_type{storage_policy, typename alloc_traits::template rebind_alloc<Entity>{allocator}},
          payload{typename alloc_traits::template rebind_alloc<typename alloc_traits::pointer>{allocator}},
          alloc{allocator} {}

    basic_storage(basic_storage &&other) noexcept
        : base_type{stl::move(other)},
          payload{stl::move(other.payload)},
          alloc{other.alloc} {
        other.payload.clear();
    }

    ~basic_storage() override {
        shutdown();
    }

    template<typename... Args>
    Type &emplace(const entity_type entt, Args &&...args) {
        const auto pos = base_type::push(entt);
        auto *slot = assure_at_least(pos);
        alloc_traits::construct(alloc, slot, stl::forward<Args>(args)...);
        return *slot;
    }

    [[nodiscard]] Type &get(const entity_type entt) noexcept { return element_at(base_type::index(entt)); }
    [[nodiscard]] const Type &get(const entity_type entt) const noexcept { return element_at(base_type::index(entt)); }

    [[nodiscard]] Type *try_get(const entity_type entt) noexcept {
        return base_type::contains(entt) ? &get(entt) : nullptr;
    }

    /** The composition point. A non-empty type contributes one reference. */
    [[nodiscard]] auto get_as_tuple(const entity_type entt) noexcept { return stl::forward_as_tuple(get(entt)); }
    [[nodiscard]] auto get_as_tuple(const entity_type entt) const noexcept { return stl::forward_as_tuple(get(entt)); }

    [[nodiscard]] iterator begin() noexcept {
        return iterator{&payload, static_cast<stl::ptrdiff_t>(base_type::size())};
    }

    [[nodiscard]] iterator end() noexcept { return iterator{&payload, 0}; }

    [[nodiscard]] const_iterator begin() const noexcept {
        return const_iterator{&payload, static_cast<stl::ptrdiff_t>(base_type::size())};
    }

    [[nodiscard]] const_iterator end() const noexcept { return const_iterator{&payload, 0}; }

    /**
     * `for(auto [entity, value] : storage.each())`.
     *
     * Zips the two iterators rather than looking each entity up: both arrays are
     * indexed by the same packed position, walked in the same direction.
     */
    [[nodiscard]] auto each() noexcept {
        using it_type = internal::extended_storage_iterator<typename base_type::iterator, iterator>;
        return internal::iterable_adaptor<it_type>{it_type{base_type::begin(), begin()},
                                                   it_type{base_type::end(), end()}};
    }

private:
    [[nodiscard]] Type &element_at(const size_type pos) const noexcept {
        return payload[pos / page_size][fast_mod(pos, page_size)];
    }

    [[nodiscard]] Type *assure_at_least(const size_type pos) {
        const auto page = pos / page_size;

        if(!(page < payload.size())) {
            payload.resize(page + 1u, nullptr);
        }

        if(payload[page] == nullptr) {
            // Raw storage only: elements are constructed one at a time, as they
            // are emplaced. The page itself is never reallocated -- that is
            // what buys pointer stability.
            payload[page] = alloc_traits::allocate(alloc, page_size);
        }

        return payload[page] + fast_mod(pos, page_size);
    }

    void shutdown() noexcept {
        // Destroy live elements before releasing the pages. Tombstoned slots
        // hold no object, so they must be skipped.
        for(size_type pos = base_type::size(); pos != 0u; --pos) {
            if(!base_type::is_tombstone((*this)[pos - 1u])) {
                alloc_traits::destroy(alloc, &element_at(pos - 1u));
            }
        }

        for(auto &page: payload) {
            if(page != nullptr) {
                alloc_traits::deallocate(alloc, page, page_size);
                page = nullptr;
            }
        }

        payload.clear();
    }

    // ---- the seam ---------------------------------------------------------

    [[nodiscard]] const void *get_at(const size_type pos) const override { return &element_at(pos); }

    // Both of these are only reachable under a policy that relocates elements,
    // and `storage_policy` selects in_place for exactly the types that cannot be
    // relocated. The guard is what makes that argument load-bearing instead of a
    // comment: without it, a non-movable component fails to *compile* here, on a
    // path it can never take. This is Module 2's inference closing the loop --
    // the trait decided the policy, and the policy makes the code well-formed.
    static constexpr bool relocatable =
        stl::is_move_constructible_v<Type> && stl::is_move_assignable_v<Type>;

    void swap_or_move(const size_type lhs, const size_type rhs) override {
        if constexpr(relocatable) {
            using stl::swap;
            swap(element_at(lhs), element_at(rhs));
        } else {
            (void)lhs, (void)rhs;
            unreachable_relocation();
        }
    }

    void move_into(const size_type from, const size_type to) override {
        if constexpr(relocatable) {
            alloc_traits::construct(alloc, &element_at(to), stl::move(element_at(from)));
            alloc_traits::destroy(alloc, &element_at(from));
        } else {
            (void)from, (void)to;
            unreachable_relocation();
        }
    }

    // Not `std::unreachable()`: if the argument above is ever wrong, a trap is a
    // stack trace and UB is a silent corruption.
    [[noreturn]] static void unreachable_relocation() {
        ACPP_TRAP();
        // ACPP_TRAP is __debugbreak on MSVC, which is not [[noreturn]].
        std::abort();
    }

    void destroy_at(const size_type pos) override {
        alloc_traits::destroy(alloc, &element_at(pos));
    }

    container_type payload;
    allocator_type alloc;
};

/**
 * Empty types have `page_size == 0` (Module 2), which means **no payload array
 * at all**. A tag component costs one entity id and nothing else.
 *
 * The interesting part is that the abstraction survives: `get_as_tuple` returns
 * an empty tuple, so a tag composes in `each()` through `tuple_cat` with no
 * special case anywhere downstream.
 */
template<typename Type, handle_like Entity, typename Allocator>
    requires(component_traits<Type, Entity>::page_size == 0u)
class basic_storage<Type, Entity, Allocator>
    : public basic_sparse_set<Entity, typename stl::allocator_traits<Allocator>::template rebind_alloc<Entity>> {
    using alloc_traits = stl::allocator_traits<Allocator>;
    using base_type = basic_sparse_set<Entity, typename alloc_traits::template rebind_alloc<Entity>>;

public:
    using element_type = Type;
    using value_type = Type;
    using entity_type = Entity;
    using allocator_type = Allocator;
    using size_type = stl::size_t;

    static constexpr size_type page_size = 0u;
    static constexpr deletion_policy storage_policy = deletion_policy::swap_and_pop;

    explicit basic_storage(const allocator_type &allocator = {})
        : base_type{storage_policy, typename alloc_traits::template rebind_alloc<Entity>{allocator}} {}

    template<typename... Args>
    void emplace(const entity_type entt, Args &&...) {
        base_type::push(entt);
    }

    /** An empty tuple. tuple_cat drops it, and the view composes anyway. */
    [[nodiscard]] auto get_as_tuple(const entity_type) const noexcept { return stl::tuple<>{}; }

    /** Nothing to zip: the tuple is just the entity. */
    [[nodiscard]] auto each() noexcept {
        using it_type = internal::extended_storage_iterator<typename base_type::iterator>;
        return internal::iterable_adaptor<it_type>{it_type{base_type::begin()}, it_type{base_type::end()}};
    }
};

template<typename Type>
using storage = basic_storage<Type, stl::uint32_t>;

} // namespace acpp

#endif // ACPP_STORAGE_HPP
