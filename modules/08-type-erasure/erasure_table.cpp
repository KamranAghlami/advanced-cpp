// Module 8, exercise 1 -- the comparison table, with numbers that were measured.
//
// Five ways to call code you do not know the type of at the call site. Not a
// ctest entry: it is a measurement.
//
// This is one of the few places where linking EnTT is the point -- the exercise
// asks for entt::delegate and entt::poly in the table, and "same answer as the
// real thing" is only meaningful against the real thing.

#include <chrono>
#include <cstdio>
#include <functional>

#include <entt/poly/poly.hpp>
#include <entt/signal/delegate.hpp>

#include <acpp/delegate.hpp>

namespace {

// --- the five callables -----------------------------------------------------

int free_function(int value) noexcept {
    return value * 3 + 1;
}

// A second implementation of everything, and a runtime choice between them.
// Without this the optimiser knows the dynamic type of every target in this
// file and devirtualises the lot -- which is how the first version of this
// table ended up reporting a virtual call as five times faster than a raw
// function pointer. The mechanisms exist for the case where the target is not
// known; the benchmark has to reproduce that case.
int other_function(int value) noexcept {
    return value * 3 + 1;
}

struct base {
    virtual ~base() = default;
    virtual int call(int value) const noexcept = 0;
};

struct derived final: base {
    int call(int value) const noexcept override { return value * 3 + 1; }
};

struct other_derived final: base {
    int call(int value) const noexcept override { return value * 3 + 1; }
};

struct callable {
    [[nodiscard]] int call(int value) const noexcept { return value * 3 + 1; }
};

// entt::poly's interface: a concept as a template, satisfied non-intrusively.
// The signature is `int(int) const`, not `int(int)`. EnTT's poly_call picks the
// const overload from inside a const member, and that overload hands the vtable
// a `const basic_any &` -- which only type-checks if the declared signature is
// const-qualified. Getting this wrong is a wall of template errors pointing at
// poly.hpp rather than at the interface.
struct poly_interface: entt::type_list<int(int) const> {
    template<typename Base>
    struct type: Base {
        [[nodiscard]] int call(int value) const {
            return entt::poly_call<0>(*this, value);
        }
    };

    template<typename Type>
    using impl = entt::value_list<&Type::call>;
};

// --- timing -----------------------------------------------------------------

constexpr int iterations = 5'000'000;
volatile int sink = 0;

/**
 * Tell the compiler an object may have been modified through a pointer it
 * cannot see.
 *
 * Without this the benchmark measures the optimiser, not the mechanism: every
 * target here is known at compile time, so gcc devirtualises the virtual call,
 * constant-folds the function pointer and inlines the std::function -- and the
 * table comes out with a raw function pointer *slower* than a delegate, which
 * is not a fact about dispatch. Barriers restore the situation the mechanisms
 * actually exist for: a callable whose target the call site does not know.
 */
void clobber(const void *pointer) {
    asm volatile("" : : "g"(pointer) : "memory");
}

struct timing {
    double best;
    double worst;
};

template<typename Fn>
[[nodiscard]] timing ns_per_call(const Fn &fn) {
    clobber(&fn);

    int total = 0;
    for(int i = 0; i < 200'000; ++i) {
        total += fn(i);
    }
    sink = total;

    // Best of five, not the mean. On a single-core box with other processes on
    // it, a slow run means something else got the CPU; it carries no
    // information about the mechanism. The minimum is the least-contaminated
    // estimate of the cost, and it is the one that reproduces.
    double best = 1e300;
    double worst = 0.0;

    for(int pass = 0; pass < 7; ++pass) {
        clobber(&fn);
        const auto started = std::chrono::steady_clock::now();
        total = 0;
        for(int i = 0; i < iterations; ++i) {
            total += fn(i);
        }
        const auto finished = std::chrono::steady_clock::now();
        sink = total;

        const auto ns = std::chrono::duration<double, std::nano>{finished - started}.count() / iterations;
        best = ns < best ? ns : best;
        worst = ns > worst ? ns : worst;
    }

    return {best, worst};
}

// The floor. Whatever this costs is loop overhead and the dependency chain on
// `total`, not dispatch -- so every row below should be read as "baseline plus
// the mechanism". Without it the table has no zero.
[[nodiscard]] timing baseline() {
    static const auto identity = [](int value) { return value * 3 + 1; };
    return ns_per_call(identity);
}

std::size_t allocations = 0u;

} // namespace

#if defined __SANITIZE_ADDRESS__ || defined __SANITIZE_THREAD__
#    define ACPP_SANITIZED 1
#elif defined __has_feature
#    if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#        define ACPP_SANITIZED 1
#    endif
#endif

#if !defined ACPP_SANITIZED
void *operator new(std::size_t bytes) {
    ++allocations;
    return std::malloc(bytes);
}

void operator delete(void *pointer) noexcept { std::free(pointer); }
void operator delete(void *pointer, std::size_t) noexcept { std::free(pointer); }
#endif

int main(int argc, char **) {
    // argc is 1, but the compiler cannot know that.
    const bool first = (argc == 1);

    callable target;
    derived one;
    other_derived two;
    const base *virtual_call = first ? static_cast<const base *>(&one) : &two;
    int (*raw)(int) noexcept = first ? &free_function : &other_function;

    // Constructed outside the timing loop, and each construction is watched so
    // "does it allocate?" is a measurement rather than a recollection.
    const auto before_fn = allocations;
    const std::function<int(int)> std_fn{raw};
    const auto std_fn_allocs = allocations - before_fn;

    const auto before_big = allocations;
    // A capture too large for libstdc++'s 16-byte std::function buffer.
    double ballast[4]{1.0, 2.0, 3.0, 4.0};
    const std::function<int(int)> std_fn_big{
        [ballast](int value) noexcept { return value * 3 + 1 + static_cast<int>(ballast[0]) - 1; }};
    const auto std_fn_big_allocs = allocations - before_big;

    const auto before_delegate = allocations;
    // Compile-time bind, the same way entt::delegate is bound below. An earlier
    // version used the runtime connect() here and the two rows were not
    // comparable -- one had an extra indirection the other did not.
    acpp::delegate<int(int)> acpp_delegate{acpp::connect_arg<&free_function>};
    const auto delegate_allocs = allocations - before_delegate;

    entt::delegate<int(int)> entt_delegate;
    entt_delegate.connect<&free_function>();

    const auto before_poly = allocations;
    entt::poly<poly_interface> poly_value{target}; // non-const: poly_call needs a mutable handle
    const auto poly_allocs = allocations - before_poly;

    // Every holder is clobbered inside ns_per_call, so each mechanism is timed
    // through the indirection it actually has rather than through whatever the
    // optimiser could prove about this particular file.
    clobber(&raw);
    clobber(&virtual_call);
    clobber(&acpp_delegate);
    clobber(&entt_delegate);
    clobber(&poly_value);

    const auto floor = baseline();

    std::printf("machine: %s\n", "1 vCPU shared cloud instance -- see the caveat below");
    std::printf("%-24s %6s %10s %10s %10s\n", "mechanism", "sizeof", "allocs", "ns/call", "worst");
    std::printf("%-24s %6s %10s %10.3f %10.3f\n", "(baseline: inlined)", "-", "-", floor.best, floor.worst);
    const auto row = [](const char *name, std::size_t size, const char *allocs, timing t) {
        std::printf("%-24s %6zu %10s %10.3f %10.3f\n", name, size, allocs, t.best, t.worst);
    };

    char buffer[16];

    row("raw function pointer", sizeof(raw), "0", ns_per_call([&raw](int v) { return raw(v); }));
    row("virtual call", sizeof(void *), "0", ns_per_call([virtual_call](int v) { return virtual_call->call(v); }));

    std::snprintf(buffer, sizeof(buffer), "%zu", delegate_allocs);
    row("acpp::delegate", sizeof(acpp_delegate), buffer,
        ns_per_call([&acpp_delegate](int v) { return acpp_delegate(v); }));

    row("entt::delegate", sizeof(entt_delegate), "0",
        ns_per_call([&entt_delegate](int v) { return entt_delegate(v); }));

    char small_buf[16];
    std::snprintf(small_buf, sizeof(small_buf), "%zu", std_fn_allocs);
    row("std::function (small)", sizeof(std_fn), small_buf, ns_per_call([&std_fn](int v) { return std_fn(v); }));

    char big_buf[16];
    std::snprintf(big_buf, sizeof(big_buf), "%zu", std_fn_big_allocs);
    row("std::function (heap)", sizeof(std_fn_big), big_buf,
        ns_per_call([&std_fn_big](int v) { return std_fn_big(v); }));

    char poly_buf[16];
    std::snprintf(poly_buf, sizeof(poly_buf), "%zu", poly_allocs);
    row("entt::poly", sizeof(poly_value), poly_buf,
        ns_per_call([&poly_value](int v) { return poly_value->call(v); }));

    std::printf("\nCaveat: 1 shared vCPU. The spread between best and worst is the noise\n"
                "floor, and it is comparable to the differences between rows -- so the\n"
                "ns/call column ranks these mechanisms only where the gap exceeds it.\n"
                "sizeof and allocs are exact; the codegen probes are the other hard evidence.\n");

#if defined ACPP_SANITIZED
    std::printf("\n(allocation counts are unavailable under sanitizers)\n");
#endif

    return 0;
}
