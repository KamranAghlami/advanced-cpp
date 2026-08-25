#ifndef ACPP_ANY_HPP
#define ACPP_ANY_HPP

// Module 8.1 -- type erasure with a small-buffer optimisation and a
// single-function vtable.
//
// Two ideas worth stealing, and they are independent:
//
//   * ONE function pointer, not N. Every operation is a case in one switch, so
//     an `any` is two pointers plus the buffer instead of a table pointer per
//     operation -- and the compiler sees all the operations for a type together,
//     which is better for inlining and for dead-stripping the ones nobody calls.
//
//   * OWNERSHIP MODE as a separate enum. The same machinery holds a value, a
//     reference or a const reference, so you do not need three wrapper types.

#include "stl/concepts.hpp"
#include "stl/cstddef.hpp"
#include "stl/cstdint.hpp"
#include "stl/memory.hpp"
#include "stl/new.hpp"
#include "stl/type_traits.hpp"
#include "stl/utility.hpp"
#include "type_info.hpp"
#include "type_traits.hpp"

namespace acpp {

/** How the object is owned. Orthogonal to what it is. */
enum class any_policy : stl::uint8_t {
    empty,    //< nothing held
    dynamic,  //< owned, heap allocated
    embedded, //< owned, inside the buffer
    ref,      //< aliasing a non-const object we do not own
    cref,     //< aliasing a const object we do not own
};

namespace internal {

/** The operations. One switch, one function pointer. */
enum class any_operation : stl::uint8_t {
    info,
    copy,
    move,
    destroy,
    compare,
};

template<stl::size_t Len, stl::size_t Align>
struct any_storage {
    union {
        const void *instance;
        alignas(Align) stl::byte buffer[Len];
    };

    any_storage() noexcept
        : instance{nullptr} {}
};

/**
 * Len == 0 specialises to a pointer-only `any`: no buffer at all.
 *
 * Exactly what you want when everything is heap-allocated anyway, and it makes
 * sizeof(any) two pointers. Not a degenerate case -- a deliberate one.
 */
template<stl::size_t Align>
struct any_storage<0u, Align> {
    const void *instance{nullptr};
};

/** Does this type fit inline, and is moving it safe enough to do so? */
template<typename Type, stl::size_t Len, stl::size_t Align>
struct in_situ
    : stl::bool_constant<(Len != 0u) && alignof(Type) <= Align && sizeof(Type) <= Len
                         && stl::is_nothrow_move_constructible_v<Type>> {};

// The nothrow-move requirement is not fussiness: moving an embedded object means
// moving the buffer, and a throwing move mid-relocation leaves two half-objects
// and no way back. Heap-allocated objects move by pointer swap, which cannot
// throw -- so a throwing-move type is pushed to the heap on purpose.

} // namespace internal

template<stl::size_t Len, stl::size_t Align>
class basic_any: private internal::any_storage<Len, Align> {
    using base_type = internal::any_storage<Len, Align>;
    using operation = internal::any_operation;
    using vtable_type = const void *(const operation, const basic_any &, const void *);

    template<typename Type>
    static constexpr bool in_situ_v = internal::in_situ<Type, Len, Align>::value;

    // THE vtable: one function, one switch, every operation for `Type` in one
    // place. `using enum` keeps the case labels unqualified without leaking the
    // enumerators into any wider scope -- C++20, in the wild.
    template<typename Type>
    static const void *basic_vtable(const operation op, const basic_any &value, const void *other) {
        const auto *elem = static_cast<const Type *>(value.data());

        switch(op) {
            using enum internal::any_operation;

        case info:
            return &type_id<Type>();

        case copy:
            if constexpr(stl::is_copy_constructible_v<Type>) {
                static_cast<basic_any *>(const_cast<void *>(other))->initialize<Type>(*elem);
                return other;
            }
            break;

        case move:
            if constexpr(in_situ_v<Type>) {
                return ::new(&static_cast<basic_any *>(const_cast<void *>(other))->buffer)
                    Type{stl::move(*const_cast<Type *>(elem))};
            }
            break;

        case destroy:
            if constexpr(in_situ_v<Type>) {
                (value.mode == any_policy::embedded) ? elem->~Type() : delete elem;
            } else {
                delete elem;
            }
            break;

        case compare:
            if constexpr(requires(const Type &lhs, const Type &rhs) {
                             { lhs == rhs } -> stl::convertible_to<bool>;
                         }) {
                return (*elem == *static_cast<const Type *>(other)) ? other : nullptr;
            } else {
                // No operator==: fall back to identity, which is the only
                // answer that is never wrong.
                return (elem == other) ? other : nullptr;
            }
        }

        return nullptr;
    }

    template<typename Type, typename... Args>
    void initialize(Args &&...args) {
        using plain_type = stl::remove_cvref_t<Type>;

        vtable = &basic_vtable<plain_type>;

        if constexpr(stl::is_lvalue_reference_v<Type>) {
            static_assert(sizeof...(Args) == 1u, "a reference binds to exactly one object");
            mode = stl::is_const_v<stl::remove_reference_t<Type>> ? any_policy::cref : any_policy::ref;
            this->instance = stl::addressof(args...);
        } else if constexpr(in_situ_v<plain_type>) {
            mode = any_policy::embedded;
            ::new(&this->buffer) plain_type{stl::forward<Args>(args)...};
        } else {
            mode = any_policy::dynamic;
            this->instance = new plain_type{stl::forward<Args>(args)...};
        }
    }

public:
    using size_type = stl::size_t;

    static constexpr size_type length = Len;
    static constexpr size_type alignment = Align;

    constexpr basic_any() noexcept = default;

    template<typename Type, typename... Args>
        requires(!stl::is_same_v<stl::remove_cvref_t<Type>, basic_any>)
    explicit basic_any(stl::in_place_type_t<Type>, Args &&...args) {
        initialize<Type>(stl::forward<Args>(args)...);
    }

    template<typename Type>
        requires(!stl::is_same_v<stl::remove_cvref_t<Type>, basic_any>)
    basic_any(Type &&value) {
        initialize<stl::remove_cvref_t<Type>>(stl::forward<Type>(value));
    }

    basic_any(const basic_any &other) {
        if(other.vtable != nullptr) {
            if(other.mode == any_policy::ref || other.mode == any_policy::cref) {
                // Aliasing modes copy the alias, not the object.
                vtable = other.vtable;
                mode = other.mode;
                this->instance = other.instance;
            } else {
                other.vtable(operation::copy, other, this);
            }
        }
    }

    basic_any(basic_any &&other) noexcept {
        if(other.vtable != nullptr) {
            vtable = other.vtable;
            mode = other.mode;

            if(other.mode == any_policy::embedded) {
                other.vtable(operation::move, other, this);
                other.vtable(operation::destroy, other, nullptr);
            } else {
                this->instance = other.instance;
            }

            other.vtable = nullptr;
            other.mode = any_policy::empty;
            other.instance = nullptr;
        }
    }

    ~basic_any() { reset(); }

    basic_any &operator=(const basic_any &other) {
        if(this != &other) {
            reset();
            basic_any copy{other};
            *this = stl::move(copy);
        }
        return *this;
    }

    basic_any &operator=(basic_any &&other) noexcept {
        if(this != &other) {
            reset();
            new(this) basic_any{stl::move(other)};
        }
        return *this;
    }

    void reset() noexcept {
        // Only owning modes destroy. An `embedded` trivially-destructible type
        // skips the call entirely -- checked at compile time, inside the vtable.
        if(vtable != nullptr && (mode == any_policy::dynamic || mode == any_policy::embedded)) {
            vtable(operation::destroy, *this, nullptr);
        }

        vtable = nullptr;
        mode = any_policy::empty;
        this->instance = nullptr;
    }

    template<typename Type, typename... Args>
    void emplace(Args &&...args) {
        reset();
        initialize<Type>(stl::forward<Args>(args)...);
    }

    [[nodiscard]] any_policy policy() const noexcept { return mode; }
    [[nodiscard]] bool owner() const noexcept {
        return mode == any_policy::dynamic || mode == any_policy::embedded;
    }

    [[nodiscard]] const type_info &type() const noexcept {
        static const type_info none{};
        return vtable != nullptr ? *static_cast<const type_info *>(vtable(operation::info, *this, nullptr)) : none;
    }

    [[nodiscard]] const void *data() const noexcept {
        if constexpr(Len == 0u) {
            return this->instance;
        } else {
            return mode == any_policy::embedded ? &this->buffer : this->instance;
        }
    }

    [[nodiscard]] void *data() noexcept {
        // A cref alias is const; handing back a mutable pointer would launder
        // the const away, so it does not.
        return mode == any_policy::cref ? nullptr : const_cast<void *>(stl::as_const(*this).data());
    }

    [[nodiscard]] explicit operator bool() const noexcept { return vtable != nullptr; }

    [[nodiscard]] bool operator==(const basic_any &other) const noexcept {
        if(vtable == nullptr || other.vtable == nullptr) {
            return vtable == other.vtable;
        }

        return type() == other.type() && vtable(operation::compare, *this, other.data()) != nullptr;
    }

    /** A non-owning alias. Same type, same machinery, different policy. */
    [[nodiscard]] basic_any as_ref() noexcept {
        basic_any result;
        result.vtable = vtable;
        result.mode = (mode == any_policy::cref) ? any_policy::cref : any_policy::ref;
        result.instance = data();
        return result;
    }

    [[nodiscard]] basic_any as_cref() const noexcept {
        basic_any result;
        result.vtable = vtable;
        result.mode = any_policy::cref;
        result.instance = data();
        return result;
    }

private:
    vtable_type *vtable{};
    any_policy mode{any_policy::empty};
};

using any = basic_any<sizeof(double[2]), alignof(double[2])>;

/** Pointer-only: no buffer, two pointers total. */
using shallow_any = basic_any<0u, alignof(void *)>;

template<typename Type, stl::size_t Len, stl::size_t Align>
[[nodiscard]] const Type *any_cast(const basic_any<Len, Align> *value) noexcept {
    return (value != nullptr && value->type() == type_id<Type>())
               ? static_cast<const Type *>(value->data())
               : nullptr;
}

template<typename Type, stl::size_t Len, stl::size_t Align>
[[nodiscard]] Type *any_cast(basic_any<Len, Align> *value) noexcept {
    return (value != nullptr && value->type() == type_id<Type>())
               ? static_cast<Type *>(value->data())
               : nullptr;
}

template<typename Type, stl::size_t Len, stl::size_t Align>
[[nodiscard]] Type any_cast(const basic_any<Len, Align> &value) {
    const auto *elem = any_cast<stl::remove_cvref_t<Type>>(&value);
    return static_cast<Type>(*elem);
}

} // namespace acpp

#endif // ACPP_ANY_HPP
