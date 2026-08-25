#ifndef ACPP_VIEW_HPP
#define ACPP_VIEW_HPP

// Module 7 -- multi-component views.
//
// A view iterates ONE storage and filters against the rest. Which one it picks
// matters enormously: iterate the 1M-element pool instead of the 12-element one
// and you do 1M lookups instead of 12. So the view scans the candidates and
// leads with the smallest.
//
// The filter chain is written so that a single-component view compiles it away
// entirely -- `Get == 1u` and `Exclude == 0u` are compile-time constants, not
// runtime checks.

#include "sparse_set.hpp"
#include "type_traits.hpp"
#include "stl/algorithm.hpp"
#include "stl/array.hpp"
#include "stl/cstddef.hpp"
#include "stl/tuple.hpp"
#include "stl/type_traits.hpp"
#include "stl/utility.hpp"
#include "storage.hpp"

namespace acpp {

template<typename... Type>
struct get_t final {
    static constexpr stl::size_t size = sizeof...(Type);
};

template<typename... Type>
struct get_t<type_list<Type...>> final {
    static constexpr stl::size_t size = sizeof...(Type);
};

template<typename... Type>
struct exclude_t final {
    static constexpr stl::size_t size = sizeof...(Type);
};

namespace internal {

/**
 * A non-null sentinel for "bound to nothing".
 *
 * An exclusion pool that does not exist must behave as "excludes nothing" -- but
 * you also need to tell "not yet bound" from "bound to an absent pool". A
 * dedicated non-null address does that with no extra bool per slot, and it means
 * every filter slot can be dereferenced unconditionally: the placeholder is a
 * real, permanently empty set.
 */
template<typename Type>
[[nodiscard]] const Type *view_placeholder() noexcept {
    static const Type placeholder{};
    return &placeholder;
}

// Qualified at every call site: these take std::array iterators, so an
// unqualified call also finds std::all_of / std::none_of through ADL and is
// ambiguous. A real trap when a library re-exports standard names.
template<typename It, typename Entity>
[[nodiscard]] bool all_of(It first, const It last, const Entity entt) noexcept {
    for(; first != last && (*first)->contains(entt); ++first) {}
    return first == last;
}

template<typename It, typename Entity>
[[nodiscard]] bool none_of(It first, const It last, const Entity entt) noexcept {
    for(; first != last && !(*first)->contains(entt); ++first) {}
    return first == last;
}

template<typename Common, stl::size_t Get, stl::size_t Exclude>
class view_iterator final {
    using iterator_type = Common::iterator;
    using entity_type = Common::value_type;

    [[nodiscard]] bool valid(const entity_type entt) const noexcept {
        // Three clauses, and the first and third vanish at compile time for a
        // single-component view with no exclusions. Note the two ranges skipping
        // `index`: there is no point re-checking the pool being iterated.
        return (!(*pools[static_cast<stl::size_t>(index)]).is_tombstone(entt))
               && ((Get == 1u)
                   || (internal::all_of(pools.begin(), pools.begin() + index, entt)
                       && internal::all_of(pools.begin() + index + 1, pools.end(), entt)))
               && ((Exclude == 0u) || internal::none_of(filter.begin(), filter.end(), entt));
    }

    void seek_next() {
        while(it != last && !valid(*it)) {
            ++it;
        }
    }

public:
    using value_type = entity_type;
    using pointer = const entity_type *;
    using reference = const entity_type &;
    using difference_type = stl::ptrdiff_t;
    using iterator_category = stl::forward_iterator_tag;

    constexpr view_iterator() noexcept = default;

    view_iterator(iterator_type from, iterator_type to,
                  stl::array<const Common *, Get> value,
                  stl::array<const Common *, Exclude> excl,
                  const stl::size_t idx) noexcept
        : it{from}, last{to}, pools{value}, filter{excl}, index{static_cast<difference_type>(idx)} {
        seek_next();
    }

    view_iterator &operator++() noexcept {
        ++it;
        seek_next();
        return *this;
    }

    view_iterator operator++(int) noexcept { const auto copy = *this; return ++*this, copy; }

    [[nodiscard]] reference operator*() const noexcept { return *it; }
    [[nodiscard]] pointer operator->() const noexcept { return &*it; }

    [[nodiscard]] bool operator==(const view_iterator &other) const noexcept { return it == other.it; }

private:
    iterator_type it{};
    iterator_type last{};
    stl::array<const Common *, Get> pools{};
    stl::array<const Common *, Exclude> filter{};
    difference_type index{};
};

} // namespace internal

/**
 * A view over N storages, minus M exclusions.
 */
template<typename, typename>
class basic_view;

template<typename... Get, typename... Exclude>
class basic_view<get_t<Get...>, exclude_t<Exclude...>> {
    static constexpr stl::size_t get_count = sizeof...(Get);
    static constexpr stl::size_t exclude_count = sizeof...(Exclude);

    static_assert(get_count > 0u, "a view must observe at least one storage");

public:
    using entity_type = stl::tuple_element_t<0u, stl::tuple<Get...>>::entity_type;
    using size_type = stl::size_t;
    using common_storage = basic_sparse_set<entity_type>;
    using iterator = internal::view_iterator<common_storage, get_count, exclude_count>;

    // Get... and Exclude... are class template parameters here, not deduced, so
    // two packs in the constructor is unambiguous. It would not be in a
    // function template -- which is why there is no deduction guide.
    basic_view(Get &...value, Exclude &...excl) noexcept
        : pools{static_cast<const common_storage *>(&value)...},
          filter{static_cast<const common_storage *>(&excl)...},
          typed{&value...} {
        refresh();
    }

    /**
     * Rescan the candidates and lead with the smallest.
     *
     * Called on construction and whenever the pools may have changed size --
     * a view holds pointers, not a snapshot, so "smallest" is a property that
     * can go stale.
     */
    void refresh() noexcept {
        index = 0u;

        if constexpr(get_count > 1u) {
            for(size_type pos = 1u; pos < get_count; ++pos) {
                if(pools[pos]->size() < pools[index]->size()) {
                    index = pos;
                }
            }
        }
    }

    /** The storage actually being iterated. */
    [[nodiscard]] const common_storage *handle() const noexcept { return pools[index]; }

    /** An upper bound: the leading pool's size, before filtering. */
    [[nodiscard]] size_type size_hint() const noexcept { return pools[index]->size(); }

    [[nodiscard]] iterator begin() const noexcept {
        return iterator{handle()->begin(), handle()->end(), pools, filter, index};
    }

    [[nodiscard]] iterator end() const noexcept {
        return iterator{handle()->end(), handle()->end(), pools, filter, index};
    }

    [[nodiscard]] bool contains(const entity_type entt) const noexcept {
        return internal::all_of(pools.begin(), pools.end(), entt)
               && internal::none_of(filter.begin(), filter.end(), entt);
    }

    template<typename Type>
    [[nodiscard]] decltype(auto) get(const entity_type entt) const {
        return storage_for<Type>()->get(entt);
    }

    /** `for(auto [entity, pos, vel] : view.each())`. */
    [[nodiscard]] auto each() const noexcept {
        using it_type = internal::extended_iterator<iterator, Get *...>;
        return internal::iterable_adaptor<it_type>{
            it_type{begin(), stl::get<Get *>(typed)...},
            it_type{end(), stl::get<Get *>(typed)...}};
    }

private:
    template<typename Type>
    [[nodiscard]] auto *storage_for() const noexcept {
        return stl::get<Type *>(typed);
    }


    // Two views of the same pools: type-erased for iteration and filtering,
    // typed for payload access.
    stl::array<const common_storage *, get_count> pools{};
    stl::array<const common_storage *, exclude_count> filter{};
    stl::tuple<Get *...> typed{};
    size_type index{};
};

/**
 * `auto view = make_view(positions, velocities);`
 *
 * Exclusions need the type spelled out, because a function template cannot
 * deduce two packs from one argument list:
 *
 *   basic_view<get_t<pos_store, vel_store>, exclude_t<frozen_store>> v{p, v, f};
 */
template<typename... Get>
[[nodiscard]] auto make_view(Get &...value) noexcept {
    return basic_view<get_t<Get...>, exclude_t<>>{value...};
}

} // namespace acpp

#endif // ACPP_VIEW_HPP
