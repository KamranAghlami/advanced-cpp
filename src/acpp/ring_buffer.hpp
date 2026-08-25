#ifndef ACPP_RING_BUFFER_HPP
#define ACPP_RING_BUFFER_HPP

// Module 3, exercise 2 -- an allocator-aware container whose stateless-allocator
// instantiation costs exactly as much as the hard-coded version.
//
// Module 4, exercise 3 -- and the container the freestanding seam was applied to
// first. Every standard name below is spelled `stl::`, so this file's dependency
// on the standard library is exactly the four includes at the top and nothing
// hidden in the body.
//
// The allocator is stored as the empty half of a compressed_pair with the data
// pointer. With std::allocator it occupies no bytes; with a stateful allocator
// it occupies exactly its own size and nothing extra. Both facts are asserted in
// modules/03-layout-economy/allocator_aware_ring.cpp.

#include "compressed_pair.hpp"
#include "stl/cstddef.hpp"
#include "stl/memory.hpp"
#include "stl/type_traits.hpp"
#include "stl/utility.hpp"

namespace acpp {

/**
 * Fixed-capacity FIFO over allocator-provided storage.
 *
 * Capacity is rounded up to a power of two so that wrapping is a mask rather
 * than a modulo, and head/tail are free-running counters rather than wrapped
 * indices -- which is what makes "full" distinguishable from "empty" without
 * wasting a slot or keeping a separate count.
 */
template<typename Type, typename Allocator = stl::allocator<Type>>
class ring_buffer {
    using alloc_traits = stl::allocator_traits<Allocator>;
    static_assert(stl::is_same_v<typename alloc_traits::value_type, Type>,
                  "the allocator must allocate the container's element type");

    using pointer = alloc_traits::pointer;

    [[nodiscard]] static constexpr stl::size_t round_up(const stl::size_t value) noexcept {
        stl::size_t result = 1u;
        while(result < value) {
            result <<= 1u;
        }
        return result;
    }

public:
    using value_type = Type;
    using allocator_type = Allocator;
    using size_type = stl::size_t;

    explicit ring_buffer(const size_type request, const allocator_type &allocator = allocator_type{})
        : storage{allocator, nullptr}, mask{round_up(request ? request : 1u) - 1u} {
        storage.second() = alloc_traits::allocate(storage.first(), capacity());
    }

    ring_buffer(const ring_buffer &) = delete;
    ring_buffer &operator=(const ring_buffer &) = delete;

    ring_buffer(ring_buffer &&other) noexcept
        : storage{stl::move(other.storage)}, mask{other.mask}, head{other.head}, tail{other.tail} {
        other.storage.second() = nullptr;
        other.mask = 0u;
        other.head = other.tail = 0u;
    }

    ring_buffer &operator=(ring_buffer &&other) noexcept {
        if(this != &other) {
            release();
            storage = stl::move(other.storage);
            mask = other.mask;
            head = other.head;
            tail = other.tail;
            other.storage.second() = nullptr;
            other.mask = 0u;
            other.head = other.tail = 0u;
        }
        return *this;
    }

    ~ring_buffer() { release(); }

    [[nodiscard]] size_type capacity() const noexcept { return mask + 1u; }
    [[nodiscard]] size_type size() const noexcept { return tail - head; }
    [[nodiscard]] bool empty() const noexcept { return head == tail; }
    [[nodiscard]] bool full() const noexcept { return size() == capacity(); }

    [[nodiscard]] allocator_type get_allocator() const noexcept { return storage.first(); }

    template<typename... Args>
    bool emplace(Args &&...args) {
        if(full()) {
            return false;
        }

        alloc_traits::construct(storage.first(), slot(tail), stl::forward<Args>(args)...);
        ++tail;
        return true;
    }

    bool push(const Type &value) { return emplace(value); }
    bool push(Type &&value) { return emplace(stl::move(value)); }

    [[nodiscard]] Type &front() noexcept { return *slot(head); }
    [[nodiscard]] const Type &front() const noexcept { return *slot(head); }

    void pop() noexcept {
        alloc_traits::destroy(storage.first(), slot(head));
        ++head;
    }

private:
    [[nodiscard]] pointer slot(const size_type counter) noexcept {
        return storage.second() + (counter & mask);
    }

    [[nodiscard]] const Type *slot(const size_type counter) const noexcept {
        return storage.second() + (counter & mask);
    }

    void release() noexcept {
        if(storage.second() != nullptr) {
            while(!empty()) {
                pop();
            }
            alloc_traits::deallocate(storage.first(), storage.second(), capacity());
            storage.second() = nullptr;
        }
    }

    // The whole point of the module, in one member: allocator and data pointer
    // in one object, and the allocator disappears when it is empty.
    compressed_pair<allocator_type, pointer> storage;
    size_type mask;
    size_type head{};
    size_type tail{};
};

} // namespace acpp

#endif // ACPP_RING_BUFFER_HPP
