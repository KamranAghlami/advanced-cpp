#ifndef ACPP_SMALL_VECTOR_HPP
#define ACPP_SMALL_VECTOR_HPP

// Module 11 -- a vector with inline capacity.
//
// Most task nodes have four edges or fewer, so a graph built from them should
// allocate nothing for its topology. That is the entire justification: it is not
// "vectors are slow", it is "the distribution of sizes in this workload has a
// short head, and the head fits in the object".
//
// Taskflow vendors LLVM's SmallVector for this. This is a much smaller thing
// with the same idea: N elements inline, heap only beyond that.

#include "stl/algorithm.hpp"
#include "stl/cstddef.hpp"
#include "stl/memory.hpp"
#include "stl/new.hpp"
#include "stl/type_traits.hpp"
#include "stl/utility.hpp"

namespace acpp {

template<typename Type, stl::size_t Inline = 4u>
class small_vector {
    static_assert(stl::is_nothrow_move_constructible_v<Type>,
                  "growing relocates; a throwing move would leave the vector half-moved");

public:
    using value_type = Type;
    using size_type = stl::size_t;
    using iterator = Type *;
    using const_iterator = const Type *;
    using reference = Type &;
    using const_reference = const Type &;

    static constexpr size_type inline_capacity = Inline;

    constexpr small_vector() noexcept
        : store{reinterpret_cast<Type *>(buffer)}, count{0u}, cap{Inline} {}

    small_vector(const small_vector &other)
        : small_vector{} {
        reserve(other.count);
        for(size_type pos = 0u; pos < other.count; ++pos) {
            push_back(other.store[pos]);
        }
    }

    small_vector(small_vector &&other) noexcept
        : small_vector{} {
        adopt(other);
    }

    small_vector &operator=(const small_vector &other) {
        if(this != &other) {
            clear();
            reserve(other.count);
            for(size_type pos = 0u; pos < other.count; ++pos) {
                push_back(other.store[pos]);
            }
        }
        return *this;
    }

    small_vector &operator=(small_vector &&other) noexcept {
        if(this != &other) {
            release();
            adopt(other);
        }
        return *this;
    }

    ~small_vector() { release(); }

    [[nodiscard]] size_type size() const noexcept { return count; }
    [[nodiscard]] size_type capacity() const noexcept { return cap; }
    [[nodiscard]] bool empty() const noexcept { return count == 0u; }

    /** True while the elements still live inside the object. */
    [[nodiscard]] bool inlined() const noexcept {
        return store == reinterpret_cast<const Type *>(buffer);
    }

    [[nodiscard]] iterator begin() noexcept { return store; }
    [[nodiscard]] const_iterator begin() const noexcept { return store; }
    [[nodiscard]] iterator end() noexcept { return store + count; }
    [[nodiscard]] const_iterator end() const noexcept { return store + count; }

    [[nodiscard]] reference operator[](const size_type pos) noexcept { return store[pos]; }
    [[nodiscard]] const_reference operator[](const size_type pos) const noexcept { return store[pos]; }
    [[nodiscard]] reference back() noexcept { return store[count - 1u]; }
    [[nodiscard]] const_reference back() const noexcept { return store[count - 1u]; }
    [[nodiscard]] Type *data() noexcept { return store; }
    [[nodiscard]] const Type *data() const noexcept { return store; }

    void reserve(const size_type request) {
        if(request > cap) {
            grow(request);
        }
    }

    template<typename... Args>
    reference emplace_back(Args &&...args) {
        if(count == cap) {
            grow(cap * 2u);
        }

        stl::construct_at(store + count, stl::forward<Args>(args)...);
        return store[count++];
    }

    void push_back(const Type &value) { emplace_back(value); }
    void push_back(Type &&value) { emplace_back(stl::move(value)); }

    void pop_back() noexcept { stl::destroy_at(store + --count); }

    void clear() noexcept {
        while(count != 0u) {
            pop_back();
        }
    }

    /** Erase by index, swapping the last element in. Order is not preserved. */
    void swap_erase(const size_type pos) noexcept {
        if(pos + 1u != count) {
            store[pos] = stl::move(store[count - 1u]);
        }

        pop_back();
    }

private:
    void grow(const size_type request) {
        const auto target = request < Inline ? Inline : request;
        auto *bigger = static_cast<Type *>(::operator new(target * sizeof(Type), std::align_val_t{alignof(Type)}));

        for(size_type pos = 0u; pos < count; ++pos) {
            stl::construct_at(bigger + pos, stl::move(store[pos]));
            stl::destroy_at(store + pos);
        }

        if(!inlined()) {
            ::operator delete(store, std::align_val_t{alignof(Type)});
        }

        store = bigger;
        cap = target;
    }

    void release() noexcept {
        clear();

        if(!inlined()) {
            ::operator delete(store, std::align_val_t{alignof(Type)});
            store = reinterpret_cast<Type *>(buffer);
            cap = Inline;
        }
    }

    void adopt(small_vector &other) noexcept {
        if(other.inlined()) {
            // Inline elements cannot be stolen -- they live inside the source
            // object. Move them one at a time.
            for(size_type pos = 0u; pos < other.count; ++pos) {
                stl::construct_at(store + pos, stl::move(other.store[pos]));
            }

            count = other.count;
            cap = Inline;
        } else {
            // Heap elements can: take the pointer and leave the source inline.
            store = other.store;
            count = other.count;
            cap = other.cap;
            other.store = reinterpret_cast<Type *>(other.buffer);
            other.cap = Inline;
        }

        other.clear();
        other.count = 0u;
    }

    alignas(Type) stl::byte buffer[Inline * sizeof(Type)];
    Type *store;
    size_type count;
    size_type cap;
};

} // namespace acpp
#endif // ACPP_SMALL_VECTOR_HPP
