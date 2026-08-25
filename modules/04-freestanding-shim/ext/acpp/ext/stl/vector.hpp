#ifndef ACPP_EXT_STL_VECTOR_HPP
#define ACPP_EXT_STL_VECTOR_HPP

// Module 4, exercise 2 -- a user-supplied replacement, picked up by
// __has_include with no build flag and no patch to the library.
//
// This is the whole point of the seam: `acpp::stl::vector` becomes a
// fixed-capacity, heap-free vector for any target that puts this directory on
// the include path ahead of nothing at all. src/acpp/stl/vector.hpp never
// changes; it just stops taking its own branch.
//
// The capacity is a build knob because a fixed-capacity container has to get it
// from somewhere, and "somewhere" on a real target is the linker script.

#include <cstddef>
#include <iterator>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

#ifndef ACPP_EXT_VECTOR_CAPACITY
#    define ACPP_EXT_VECTOR_CAPACITY 512
#endif

namespace acpp::stl {

/**
 * A contiguous iterator that is a *class*, not a pointer.
 *
 * The first version of this header used `Type *` for `iterator`, which passed
 * gcc and failed clang: EnTT's edge_iterator writes `It::difference_type`, and a
 * raw pointer has no members. That is the sharpest lesson of the exercise --
 * "looks like std::vector" includes the iterator being a class type with the
 * five member typedefs, and no amount of reading the member function list
 * reveals it. std::vector's iterator is a class in every real implementation for
 * exactly this reason (plus overload isolation from raw pointers).
 */
template<typename Type>
class contiguous_iterator {
public:
    using value_type = std::remove_const_t<Type>;
    using difference_type = std::ptrdiff_t;
    using pointer = Type *;
    using reference = Type &;
    using iterator_category = std::random_access_iterator_tag;
    using iterator_concept = std::contiguous_iterator_tag;

    constexpr contiguous_iterator() noexcept = default;

    constexpr explicit contiguous_iterator(pointer target) noexcept
        : cursor{target} {}

    // const_iterator is constructible from iterator, never the reverse.
    template<typename Other>
        requires std::is_same_v<Type, const Other>
    constexpr contiguous_iterator(const contiguous_iterator<Other> &other) noexcept
        : cursor{other.operator->()} {}

    constexpr contiguous_iterator &operator++() noexcept { return ++cursor, *this; }
    constexpr contiguous_iterator operator++(int) noexcept { const auto copy = *this; return ++*this, copy; }
    constexpr contiguous_iterator &operator--() noexcept { return --cursor, *this; }
    constexpr contiguous_iterator operator--(int) noexcept { const auto copy = *this; return --*this, copy; }

    constexpr contiguous_iterator &operator+=(const difference_type value) noexcept { cursor += value; return *this; }
    constexpr contiguous_iterator &operator-=(const difference_type value) noexcept { cursor -= value; return *this; }

    [[nodiscard]] constexpr contiguous_iterator operator+(const difference_type value) const noexcept {
        return contiguous_iterator{cursor + value};
    }

    [[nodiscard]] constexpr contiguous_iterator operator-(const difference_type value) const noexcept {
        return contiguous_iterator{cursor - value};
    }

    [[nodiscard]] friend constexpr contiguous_iterator operator+(const difference_type value,
                                                                 const contiguous_iterator other) noexcept {
        return other + value;
    }

    [[nodiscard]] constexpr difference_type operator-(const contiguous_iterator &other) const noexcept {
        return cursor - other.cursor;
    }

    [[nodiscard]] constexpr reference operator[](const difference_type value) const noexcept {
        return cursor[value];
    }

    [[nodiscard]] constexpr reference operator*() const noexcept { return *cursor; }
    [[nodiscard]] constexpr pointer operator->() const noexcept { return cursor; }

    [[nodiscard]] constexpr bool operator==(const contiguous_iterator &) const noexcept = default;
    [[nodiscard]] constexpr auto operator<=>(const contiguous_iterator &) const noexcept = default;

private:
    pointer cursor{};
};

/**
 * A vector that cannot grow.
 *
 * Deliberately narrow: it implements the subset the library actually uses, and
 * nothing else. Anything missing is a compile error naming the exact operation,
 * which is far more useful than a heap allocation nobody noticed. Overflowing
 * the capacity traps rather than reallocating -- on a target that chose this
 * header, silently allocating would be the worse failure.
 */
template<typename Type, std::size_t Capacity = ACPP_EXT_VECTOR_CAPACITY>
class fixed_vector {
    using storage_type = std::aligned_storage_t<sizeof(Type), alignof(Type)>;

public:
    // The member typedef list is not boilerplate: it is the part of the
    // std::vector interface that generic code reads rather than calls, and it
    // is what a replacement gets wrong first. Everything here was added because
    // something concrete failed to compile without it -- see NOTES.md.
    using value_type = Type;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;
    using reference = Type &;
    using const_reference = const Type &;
    using pointer = Type *;
    using const_pointer = const Type *;
    using iterator = contiguous_iterator<Type>;
    using const_iterator = contiguous_iterator<const Type>;

    // Accepted and ignored. A caller that hands us an allocator is not wrong to
    // -- it does not know which vector it got -- and refusing would mean editing
    // the library, which is the thing this header exists to avoid.
    using allocator_type = std::allocator<Type>;

    static constexpr size_type static_capacity = Capacity;

    constexpr fixed_vector() noexcept = default;

    explicit fixed_vector(const allocator_type &) noexcept {}

    explicit fixed_vector(const size_type count) {
        resize(count);
    }

    fixed_vector(const size_type count, const Type &value) {
        resize(count, value);
    }

    fixed_vector(const size_type count, const Type &value, const allocator_type &) {
        resize(count, value);
    }

    fixed_vector(const fixed_vector &other) {
        for(size_type pos = 0u; pos < other.count; ++pos) {
            push_back(other[pos]);
        }
    }

    fixed_vector(fixed_vector &&other) noexcept {
        for(size_type pos = 0u; pos < other.count; ++pos) {
            push_back(std::move(other[pos]));
        }
        other.clear();
    }

    fixed_vector &operator=(const fixed_vector &other) {
        if(this != &other) {
            clear();
            for(size_type pos = 0u; pos < other.count; ++pos) {
                push_back(other[pos]);
            }
        }
        return *this;
    }

    fixed_vector &operator=(fixed_vector &&other) noexcept {
        if(this != &other) {
            clear();
            for(size_type pos = 0u; pos < other.count; ++pos) {
                push_back(std::move(other[pos]));
            }
            other.clear();
        }
        return *this;
    }

    ~fixed_vector() { clear(); }

    [[nodiscard]] size_type size() const noexcept { return count; }
    [[nodiscard]] size_type capacity() const noexcept { return Capacity; }
    [[nodiscard]] bool empty() const noexcept { return count == 0u; }

    [[nodiscard]] iterator begin() noexcept { return iterator{data()}; }
    [[nodiscard]] const_iterator begin() const noexcept { return const_iterator{data()}; }
    [[nodiscard]] iterator end() noexcept { return iterator{data() + count}; }
    [[nodiscard]] const_iterator end() const noexcept { return const_iterator{data() + count}; }
    [[nodiscard]] const_iterator cbegin() const noexcept { return begin(); }
    [[nodiscard]] const_iterator cend() const noexcept { return end(); }

    [[nodiscard]] reference operator[](const size_type pos) noexcept { return data()[pos]; }
    [[nodiscard]] const_reference operator[](const size_type pos) const noexcept { return data()[pos]; }
    [[nodiscard]] reference back() noexcept { return data()[count - 1u]; }
    [[nodiscard]] const_reference back() const noexcept { return data()[count - 1u]; }

    [[nodiscard]] Type *data() noexcept { return reinterpret_cast<Type *>(cells); }
    [[nodiscard]] const Type *data() const noexcept { return reinterpret_cast<const Type *>(cells); }

    [[nodiscard]] allocator_type get_allocator() const noexcept { return allocator_type{}; }

    // No-ops: the capacity is already there or it is not. Present so callers do
    // not have to know which vector they got.
    void reserve(const size_type request) { overflow_check(request); }
    void shrink_to_fit() noexcept {}

    void swap(fixed_vector &other) {
        fixed_vector tmp{std::move(other)};
        other = std::move(*this);
        *this = std::move(tmp);
    }

    template<typename... Args>
    reference emplace_back(Args &&...args) {
        overflow_check(count + 1u);
        ::new(static_cast<void *>(cells + count)) Type{std::forward<Args>(args)...};
        return data()[count++];
    }

    void push_back(const Type &value) { emplace_back(value); }
    void push_back(Type &&value) { emplace_back(std::move(value)); }

    void pop_back() noexcept {
        data()[--count].~Type();
    }

    void resize(const size_type request) {
        overflow_check(request);
        while(count > request) {
            pop_back();
        }
        while(count < request) {
            emplace_back();
        }
    }

    void resize(const size_type request, const Type &value) {
        overflow_check(request);
        while(count > request) {
            pop_back();
        }
        while(count < request) {
            emplace_back(value);
        }
    }

    void clear() noexcept {
        while(count != 0u) {
            pop_back();
        }
    }

private:
    static void overflow_check(const size_type request) {
        if(request > Capacity) {
            // A target that chose a heap-free vector wants a loud, immediate
            // stop, not a silent allocation and not an exception it compiled
            // out. On a real board this is where the fault handler goes.
            __builtin_trap();
        }
    }

    alignas(Type) storage_type cells[Capacity];
    size_type count{};
};

// The name the library asks for. The allocator parameter is accepted and
// ignored -- callers pass std::allocator by default and never look at it, and
// refusing it here would mean editing the library, which is the thing this
// header exists to avoid.
template<typename Type, typename = void>
using vector = fixed_vector<Type>;

} // namespace acpp::stl

#endif // ACPP_EXT_STL_VECTOR_HPP
