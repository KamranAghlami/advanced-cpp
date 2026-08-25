#ifndef ACPP_DELEGATE_HPP
#define ACPP_DELEGATE_HPP

// Module 8.2 -- the fastest callable wrapper you can write.
//
// Two pointers: an untyped instance pointer and a function pointer with a fixed
// signature. The target is bound through a *template parameter*, so it is known
// at compile time and the trampoline is a direct call the optimiser can usually
// inline straight through.
//
// No allocation, ever. No type erasure of the callable's storage -- only of its
// identity. That is the whole trade against std::function: a delegate does not
// own anything, so the caller must guarantee the target outlives it.
//
// Where this matters: interrupt-adjacent code, driver callback tables, event
// dispatch on a target with no heap. std::function is a bad fit for all three.

#include "stl/functional.hpp"
#include "stl/type_traits.hpp"
#include "stl/utility.hpp"
#include "type_traits.hpp"

namespace acpp {

/** Tag for constructing a delegate from a compile-time target. */
template<auto Candidate>
struct connect_arg_t final {
    explicit connect_arg_t() = default;
};

template<auto Candidate>
inline constexpr connect_arg_t<Candidate> connect_arg{};

template<typename>
class delegate;

template<typename Ret, typename... Args>
class delegate<Ret(Args...)> {
    // The uniform shape every trampoline is compiled to. The instance pointer is
    // first so a free function with a payload and a member function have the
    // same calling convention.
    using proto_fn_type = Ret(const void *, Args...);

public:
    using function_type = Ret(const void *, Args...);
    using type = Ret(Args...);
    using result_type = Ret;

    constexpr delegate() noexcept = default;

    template<auto Candidate, typename... Type>
    delegate(connect_arg_t<Candidate>, Type &&...value_or_instance) noexcept {
        connect<Candidate>(stl::forward<Type>(value_or_instance)...);
    }

    delegate(function_type *function, const void *payload = nullptr) noexcept {
        connect(function, payload);
    }

    /** Free function, or a static member. No instance. */
    template<auto Candidate>
    void connect() noexcept {
        instance = nullptr;

        // The trampoline is a stateless lambda decayed to a function pointer.
        // Candidate is a template parameter, so the call inside is direct.
        fn = +[](const void *, Args... args) -> Ret {
            return static_cast<Ret>(stl::invoke(Candidate, stl::forward<Args>(args)...));
        };
    }

    /** Member function bound to an instance, or a free function with a payload
     *  passed as its first argument. */
    template<auto Candidate, typename Type>
    void connect(Type &value_or_instance) noexcept {
        instance = &value_or_instance;

        fn = +[](const void *payload, Args... args) -> Ret {
            // const_cast<void *> first: the stored pointer is `const void *`
            // whatever the instance's own const-ness, and casting straight to
            // `Type *` from `const void *` is ill-formed for non-const Type.
            auto *curr = static_cast<Type *>(const_cast<void *>(payload));
            return static_cast<Ret>(stl::invoke(Candidate, *curr, stl::forward<Args>(args)...));
        };
    }

    /** Runtime-known function pointer. The escape hatch; no inlining here. */
    void connect(function_type *function, const void *payload = nullptr) noexcept {
        instance = payload;
        fn = function;
    }

    void reset() noexcept {
        instance = nullptr;
        fn = nullptr;
    }

    [[nodiscard]] function_type *target() const noexcept { return fn; }
    [[nodiscard]] const void *data() const noexcept { return instance; }

    [[nodiscard]] explicit operator bool() const noexcept { return fn != nullptr; }

    // Deliberately no null check. A delegate is the low-overhead option; adding
    // a branch on every call to catch a programming error would defeat it. The
    // contract is "connect before you call", the same contract a raw function
    // pointer has.
    Ret operator()(Args... args) const {
        return fn(instance, stl::forward<Args>(args)...);
    }

    [[nodiscard]] bool operator==(const delegate &other) const noexcept {
        return fn == other.fn && instance == other.instance;
    }

private:
    const void *instance{};
    proto_fn_type *fn{};
};

// No deduction guide. Recovering Ret(Args...) from an arbitrary `auto Candidate`
// -- which may be a free function, a member function pointer, or a pointer to
// data -- means reimplementing the whole signature algebra for a convenience
// that saves one line at the call site. Spell the type:
//
//   acpp::delegate<void(int)> on_tick{acpp::connect_arg<&handler::tick>, obj};

} // namespace acpp

#endif // ACPP_DELEGATE_HPP
