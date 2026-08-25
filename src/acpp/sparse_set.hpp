#ifndef ACPP_SPARSE_SET_HPP
#define ACPP_SPARSE_SET_HPP

// Module 6 -- the sparse set.
//
// Two arrays and one invariant:
//
//   packed[sparse[e]] == e
//
// `packed` is dense and contiguous -- iterate that. `sparse` is indexed by
// entity index and holds the position into `packed` -- look up through that,
// in O(1). Everything else in this file is layered on top of those two lines.
//
// What makes the design worth studying is the three deletion policies, the
// paged sparse array, and the free list that lives inside the array it manages.

#include "bit.hpp"
#include "config.hpp"
#include "handle.hpp"
#include "stl/cstddef.hpp"
#include "stl/iterator.hpp"
#include "stl/memory.hpp"
#include "stl/type_traits.hpp"
#include "stl/utility.hpp"
#include "stl/vector.hpp"

namespace acpp {

/**
 * How erasure behaves. Each exists for a concrete reason; see NOTES.md.
 */
enum class deletion_policy : stl::uint8_t {
    swap_and_pop, //< move the last element into the hole and shrink. Dense, no
                  //  holes, invalidates the moved element's position.
    in_place,     //< leave a hole and thread it onto a free list. Positions are
                  //  stable for the element's whole lifetime.
    swap_only,    //< never destroy: partition live below `head`, dead above.
};

namespace internal {

/**
 * Random access iterator over the packed array, running **backward**.
 *
 * `begin()` sits at the high end and `++` walks toward index 0. Two guarantees
 * fall out of that one choice, and they are the same decision seen from two
 * directions:
 *
 *   * erasing the current element under swap-and-pop is safe -- the element
 *     swapped into the hole comes from the highest live index, which is a slot
 *     this iterator has already passed, so nothing is skipped;
 *   * elements created mid-iteration are appended at high packed indices, which
 *     are *behind* begin(), so they are not visited.
 *
 * Both are proved in modules/07-storage-and-views rather than asserted here.
 */
template<typename Container>
class sparse_set_iterator {
    friend class sparse_set_iterator<const Container>;

    using container_type = Container;
    using alloc_traits = stl::allocator_traits<typename container_type::allocator_type>;

public:
    using value_type = container_type::value_type;
    using pointer = alloc_traits::const_pointer;
    using reference = const value_type &;
    using difference_type = stl::ptrdiff_t;
    using iterator_category = stl::random_access_iterator_tag;

    constexpr sparse_set_iterator() noexcept = default;

    constexpr sparse_set_iterator(const container_type &ref, const difference_type idx) noexcept
        : packed{&ref}, offset{idx} {}

    // The reversal is not cosmetic: every relational operator below is flipped
    // to match, so `it1 < it2` still means "it1 comes first in iteration order".
    constexpr sparse_set_iterator &operator++() noexcept { return --offset, *this; }
    constexpr sparse_set_iterator operator++(int) noexcept { const auto copy = *this; return ++*this, copy; }
    constexpr sparse_set_iterator &operator--() noexcept { return ++offset, *this; }
    constexpr sparse_set_iterator operator--(int) noexcept { const auto copy = *this; return --*this, copy; }

    constexpr sparse_set_iterator &operator+=(const difference_type value) noexcept { offset -= value; return *this; }
    constexpr sparse_set_iterator &operator-=(const difference_type value) noexcept { offset += value; return *this; }

    [[nodiscard]] constexpr sparse_set_iterator operator+(const difference_type value) const noexcept {
        auto copy = *this;
        return copy += value;
    }

    [[nodiscard]] constexpr sparse_set_iterator operator-(const difference_type value) const noexcept {
        return *this + -value;
    }

    [[nodiscard]] constexpr difference_type operator-(const sparse_set_iterator &other) const noexcept {
        return other.offset - offset;
    }

    [[nodiscard]] constexpr reference operator[](const difference_type value) const noexcept {
        return (*packed)[index() - value];
    }

    [[nodiscard]] constexpr reference operator*() const noexcept { return (*packed)[index()]; }
    [[nodiscard]] constexpr pointer operator->() const noexcept { return &operator*(); }

    /** The packed position this iterator refers to. `offset - 1`, not `offset`. */
    [[nodiscard]] constexpr stl::size_t index() const noexcept {
        return static_cast<stl::size_t>(offset - 1);
    }

    [[nodiscard]] constexpr bool operator==(const sparse_set_iterator &other) const noexcept {
        return offset == other.offset;
    }

    [[nodiscard]] constexpr auto operator<=>(const sparse_set_iterator &other) const noexcept {
        return other.offset <=> offset; // flipped, deliberately
    }

private:
    const container_type *packed{};
    difference_type offset{};
};

} // namespace internal

/**
 * A sparse set over entity identifiers, with no knowledge of any payload.
 *
 * The private-virtual seam at the bottom is what lets it stay that way: the base
 * owns the algorithm (which slot moves where) and the derived class owns one
 * step of it (keeping a parallel payload array in sync). Non-Virtual Interface,
 * textbook use.
 */
template<handle_like Entity, typename Allocator = stl::allocator<Entity>>
class basic_sparse_set {
    using alloc_traits = stl::allocator_traits<Allocator>;
    static_assert(stl::is_same_v<typename alloc_traits::value_type, Entity>, "invalid value type");

    using traits_type = handle_traits<Entity>;
    using entity_type = traits_type::entity_type;
    using sparse_container_type =
        stl::vector<typename alloc_traits::pointer,
                    typename alloc_traits::template rebind_alloc<typename alloc_traits::pointer>>;
    using packed_container_type = stl::vector<Entity, Allocator>;

public:
    using value_type = Entity;
    using allocator_type = Allocator;
    using size_type = stl::size_t;
    using difference_type = stl::ptrdiff_t;
    using iterator = internal::sparse_set_iterator<packed_container_type>;
    using const_iterator = iterator;
    using reverse_iterator = stl::reverse_iterator<iterator>;

    static constexpr size_type page_size = ACPP_SPARSE_PAGE;
    static_assert(is_valid_page_size(page_size), "the paging scheme needs a power-of-two page size");

    /** No slot. Distinct from every real position because the index space stops
     *  one short of it -- see Module 5's allocate(). */
    static constexpr size_type max_size = static_cast<size_type>(traits_type::index_mask);

    explicit basic_sparse_set(const deletion_policy pol = deletion_policy::swap_and_pop,
                              const allocator_type &allocator = {})
        : sparse{typename alloc_traits::template rebind_alloc<typename alloc_traits::pointer>{allocator}},
          packed{allocator},
          mode{pol},
          head{policy_to_head()} {}

    basic_sparse_set(const basic_sparse_set &) = delete;
    basic_sparse_set &operator=(const basic_sparse_set &) = delete;

    basic_sparse_set(basic_sparse_set &&other) noexcept
        : sparse{stl::move(other.sparse)},
          packed{stl::move(other.packed)},
          mode{other.mode},
          head{stl::exchange(other.head, other.policy_to_head())} {
        other.sparse.clear();
    }

    virtual ~basic_sparse_set() { release_sparse_pages(); }

    [[nodiscard]] deletion_policy policy() const noexcept { return mode; }
    [[nodiscard]] size_type size() const noexcept { return packed.size(); }
    [[nodiscard]] bool empty() const noexcept { return packed.empty(); }

    /** Live elements. Differs from size() for the two policies that keep holes. */
    [[nodiscard]] size_type count() const noexcept {
        switch(mode) {
        case deletion_policy::swap_and_pop:
            return packed.size();
        case deletion_policy::in_place:
            return packed.size() - free_list_length();
        case deletion_policy::swap_only:
            return head;
        }
        return 0u;
    }

    /** The head of the in-place free list, or max_size when there are no holes. */
    [[nodiscard]] size_type free_list() const noexcept { return head; }

    [[nodiscard]] bool contains(const Entity entt) const noexcept {
        const auto *elem = sparse_ptr(entt);

        // Three questions, and all three have to be asked. The version check is
        // what makes a stale handle fail; the null check is what makes an
        // erased slot fail; and for swap_only the element also has to be below
        // the partition boundary.
        constexpr auto cap = traits_type::index_mask;
        return elem != nullptr
               && ((traits_type::to_integral(*elem) & cap) != cap)
               && (traits_type::to_version(*elem) == traits_type::to_version(entt))
               && ((mode != deletion_policy::swap_only) || (entity_to_pos(*elem) < head));
    }

    /** Packed position of a contained entity. Undefined if !contains(entt). */
    [[nodiscard]] size_type index(const Entity entt) const noexcept {
        return entity_to_pos(sparse_ref(entt));
    }

    [[nodiscard]] Entity operator[](const size_type pos) const noexcept { return packed[pos]; }

    // Iteration runs from the high end toward 0; see sparse_set_iterator.
    [[nodiscard]] iterator begin() const noexcept {
        return iterator{packed, static_cast<difference_type>(packed.size())};
    }

    [[nodiscard]] iterator end() const noexcept { return iterator{packed, 0}; }
    [[nodiscard]] reverse_iterator rbegin() const noexcept { return reverse_iterator{end()}; }
    [[nodiscard]] reverse_iterator rend() const noexcept { return reverse_iterator{begin()}; }

    /** Inserts. Returns the packed position, or max_size if already present. */
    size_type push(const Entity entt) {
        if(contains(entt)) {
            return max_size;
        }

        auto &elem = assure_at_least(entt);
        size_type pos = max_size;

        switch(mode) {
        case deletion_policy::in_place:
            if(head != max_size) {
                // Reuse the most recent hole. The slot being reused currently
                // holds the *previous* hole's index in its entity bits, so
                // popping the free list is one read of packed[head].
                pos = head;
                elem = traits_type::combine(static_cast<entity_type>(head), traits_type::to_integral(entt));
                head = entity_to_pos(stl::exchange(packed[pos], entt));
                break;
            }
            [[fallthrough]];
        case deletion_policy::swap_and_pop:
            pos = packed.size();
            packed.push_back(entt);
            elem = traits_type::combine(static_cast<entity_type>(pos), traits_type::to_integral(entt));
            break;
        case deletion_policy::swap_only:
            if(elem == static_cast<Entity>(null)) {
                // Brand new: append, then move it across the boundary.
                pos = packed.size();
                packed.push_back(entt);
                elem = traits_type::combine(static_cast<entity_type>(pos), traits_type::to_integral(entt));
            } else if(traits_type::to_version(elem) != traits_type::to_version(entt)) {
                // The slot exists but at a different generation. Accepting this
                // would resurrect a handle the container has already retired.
                return max_size;
            }

            // Recycling, in one line: the released slot is sitting above the
            // boundary, and moving the boundary up brings it back. No free
            // list, no separate structure -- the partition *is* the free list.
            swap_at(entity_to_pos(elem), head++);
            pos = head - 1u;
            break;
        }

        return pos;
    }

    bool erase(const Entity entt) {
        if(!contains(entt)) {
            return false;
        }

        switch(mode) {
        case deletion_policy::swap_and_pop:
            swap_and_pop(entt);
            break;
        case deletion_policy::in_place:
            in_place_pop(entt);
            break;
        case deletion_policy::swap_only:
            swap_only(entt);
            break;
        }

        return true;
    }

    void clear() {
        for(size_type pos = packed.size(); pos != 0u; --pos) {
            if(!is_tombstone(packed[pos - 1u])) {
                destroy_at(pos - 1u);
            }
        }

        release_sparse_pages();
        sparse.clear();
        packed.clear();
        head = policy_to_head();
    }

    /**
     * Removes the holes an in-place policy left behind, restoring density.
     *
     * The cost of `in_place` is that iteration must skip tombstones; this is how
     * you pay it back once, at a point you choose, rather than on every pass.
     */
    void compact() {
        if(mode != deletion_policy::in_place) {
            return;
        }

        size_type next = packed.size();

        // Walk from the back, filling each hole with the last live element.
        while(next != 0u && is_tombstone(packed[next - 1u])) {
            --next;
        }

        for(size_type pos = 0u; pos < next; ++pos) {
            if(is_tombstone(packed[pos])) {
                --next;
                // `pos` is a hole: its payload slot holds no object, so this is
                // a move-construct, not a swap.
                move_into(next, pos);
                packed[pos] = packed[next];
                sparse_ref(packed[pos]) =
                    traits_type::combine(static_cast<entity_type>(pos), traits_type::to_integral(packed[pos]));

                while(next != 0u && is_tombstone(packed[next - 1u])) {
                    --next;
                }
            }
        }

        packed.resize(next);
        head = max_size;
    }

    [[nodiscard]] bool is_tombstone(const Entity entt) const noexcept { return tombstone == entt; }

protected:
    /** Length of the intrusive free list. O(holes); used by count() and tests. */
    [[nodiscard]] size_type free_list_length() const noexcept {
        size_type count = 0u;
        for(auto cursor = head; cursor != max_size; ++count) {
            cursor = entity_to_pos(packed[cursor]);
        }
        return count;
    }

    void swap_at(const size_type lhs, const size_type rhs) {
        auto &from = packed[lhs];
        auto &to = packed[rhs];

        sparse_ref(from) = traits_type::combine(static_cast<entity_type>(rhs), traits_type::to_integral(from));
        sparse_ref(to) = traits_type::combine(static_cast<entity_type>(lhs), traits_type::to_integral(to));

        swap_or_move(lhs, rhs);
        stl::swap(from, to);
    }

private:
    // ---- the private-virtual seam -----------------------------------------
    //
    // The base decides *which* slots move; the derived class knows what else
    // lives at those slots. Private, because no caller should reach them, and
    // virtual, because the base must call them. Non-Virtual Interface.

    /** Type-erased read of the payload at a position. Nothing here has one. */
    [[nodiscard]] virtual const void *get_at(const size_type) const { return nullptr; }

    /** Swap the payloads of two **live** slots. */
    virtual void swap_or_move(const size_type, const size_type) {}

    /** Move-construct the payload at a **vacated** slot from a live one, and
     *  destroy the source. Distinct from swap_or_move because the destination
     *  holds no object -- swapping into raw storage is not a thing. */
    virtual void move_into(const size_type, const size_type) {}

    /** Destroy the payload of a live slot that is about to be vacated. */
    virtual void destroy_at(const size_type) {}

    // ---- paging ------------------------------------------------------------

    [[nodiscard]] size_type policy_to_head() const noexcept {
        // Branchless: swap_only starts with an empty live partition (head 0),
        // the other two start with an empty free list (head max_size). Written
        // as a multiplication rather than a ternary, as in the original.
        return max_size * static_cast<size_type>(mode != deletion_policy::swap_only);
    }

    [[nodiscard]] size_type entity_to_pos(const Entity entt) const noexcept {
        return static_cast<size_type>(traits_type::to_index(entt));
    }

    [[nodiscard]] size_type pos_to_page(const size_type pos) const noexcept { return pos / page_size; }

    [[nodiscard]] auto sparse_ptr(const Entity entt) const {
        const auto pos = entity_to_pos(entt);
        const auto page = pos_to_page(pos);
        return (page < sparse.size() && sparse[page] != nullptr)
                   ? (sparse[page] + fast_mod(pos, page_size))
                   : nullptr;
    }

    [[nodiscard]] Entity &sparse_ref(const Entity entt) const {
        const auto pos = entity_to_pos(entt);
        return sparse[pos_to_page(pos)][fast_mod(pos, page_size)];
    }

    [[nodiscard]] Entity &assure_at_least(const Entity entt) {
        const auto pos = entity_to_pos(entt);
        const auto page = pos_to_page(pos);

        if(!(page < sparse.size())) {
            sparse.resize(page + 1u, nullptr);
        }

        if(sparse[page] == nullptr) {
            // Allocator-aware and exception-shaped: no `new`, and the page is
            // filled with null so an unwritten slot reads as "not present".
            auto page_allocator{packed.get_allocator()};
            sparse[page] = alloc_traits::allocate(page_allocator, page_size);
            stl::uninitialized_fill(sparse[page], sparse[page] + page_size, static_cast<Entity>(null));
        }

        return sparse[page][fast_mod(pos, page_size)];
    }

    void release_sparse_pages() {
        auto page_allocator{packed.get_allocator()};

        for(auto &page: sparse) {
            if(page != nullptr) {
                stl::destroy(page, page + page_size);
                alloc_traits::deallocate(page_allocator, page, page_size);
                page = nullptr;
            }
        }
    }

    // ---- the three policies ------------------------------------------------

    void swap_and_pop(const Entity entt) {
        auto &self = sparse_ref(entt);
        const auto pos = entity_to_pos(self);
        const auto last = packed.size() - 1u;

        if(pos != last) {
            // Swap first, destroy second. Doing it the other way round would
            // leave the derived class swapping into raw storage.
            sparse_ref(packed[last]) =
                traits_type::combine(static_cast<entity_type>(pos), traits_type::to_integral(packed[last]));
            swap_or_move(pos, last);
            packed[pos] = packed[last];
        }

        destroy_at(last);
        self = static_cast<Entity>(null);
        packed.pop_back();
    }

    /**
     * The most elegant thing in the file.
     *
     * The vacated packed slot is reused to hold *the index of the previously
     * vacated slot* in its entity bits, with `tombstone` in its version bits.
     * `head` points at the most recent hole. The free list therefore lives
     * inside the array it manages, at zero extra memory, and the tombstoned
     * version keeps holes distinguishable from live entries during iteration.
     */
    void in_place_pop(const Entity entt) {
        destroy_at(entity_to_pos(sparse_ref(entt)));
        const auto pos = entity_to_pos(stl::exchange(sparse_ref(entt), static_cast<Entity>(null)));
        packed[pos] = traits_type::combine(static_cast<entity_type>(stl::exchange(head, pos)),
                                           traits_type::to_integral(static_cast<Entity>(tombstone)));
    }

    /**
     * Nothing is destroyed. The array is partitioned at `head`: live below,
     * dead above. Releasing moves the element across the boundary and bumps its
     * version, so the partition *is* the free list -- which is why the entity
     * storage in a registry needs no separate recycling structure at all.
     */
    void swap_only(const Entity entt) {
        const auto pos = index(entt);
        bump_version(traits_type::next(entt));
        swap_at(pos, head -= static_cast<size_type>(pos < head));
    }

    void bump_version(const Entity entt) {
        auto &elem = sparse_ref(entt);
        elem = traits_type::combine(traits_type::to_integral(elem), traits_type::to_integral(entt));
        packed[entity_to_pos(elem)] = entt;
    }

    sparse_container_type sparse;
    packed_container_type packed;
    deletion_policy mode;
    size_type head;
};

using sparse_set = basic_sparse_set<stl::uint32_t>;

} // namespace acpp

#endif // ACPP_SPARSE_SET_HPP
