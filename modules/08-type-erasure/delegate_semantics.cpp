// Module 8, exercises 3 and 4 -- what a delegate is and is not.
//
// A delegate is two pointers and no ownership. Everything below is a consequence
// of that, including the parts that are worse than std::function.

#include <cstdint>
#include <functional>
#include <string>
#include <type_traits>
#include <vector>

#include <acpp/delegate.hpp>
#include <acpp/testing.hpp>

namespace {

int triple(int value) noexcept {
    return value * 3;
}

int with_payload(const std::string &prefix, int value) noexcept {
    return static_cast<int>(prefix.size()) + value;
}

struct accumulator {
    int total{};

    void add(int value) noexcept { total += value; }
    [[nodiscard]] int doubled() const noexcept { return total * 2; }
};

using delegate = acpp::delegate<int(int)>;

// Two pointers, and nothing else. This is the headline.
static_assert(sizeof(delegate) == 2u * sizeof(void *));
static_assert(std::is_trivially_copyable_v<delegate>);
static_assert(std::is_nothrow_default_constructible_v<delegate>);

// std::function is bigger, and libstdc++'s is not trivially copyable because it
// has to manage whatever it owns.
static_assert(sizeof(std::function<int(int)>) > sizeof(delegate));
static_assert(!std::is_trivially_copyable_v<std::function<int(int)>>);

} // namespace

int main() {
    acpp::testing::suite suite{"module 08 / delegate_semantics"};

    suite.note("sizeof(acpp::delegate<int(int)>) = %zu, sizeof(std::function<int(int)>) = %zu",
               sizeof(delegate), sizeof(std::function<int(int)>));

    // --- the three binding modes --------------------------------------------
    {
        delegate free_fn{acpp::connect_arg<&triple>};
        suite.check(free_fn(5) == 15, "free function");
        suite.check(static_cast<bool>(free_fn), "and reports as connected");

        accumulator target;
        acpp::delegate<void(int)> member{acpp::connect_arg<&accumulator::add>, target};
        member(4);
        member(6);
        suite.check(target.total == 10, "member function bound to an instance");

        acpp::delegate<int()> const_member{acpp::connect_arg<&accumulator::doubled>, target};
        suite.check(const_member() == 20, "const member function");

        // A free function whose first parameter is the payload. Same trampoline
        // shape as a member function, which is why both fit two pointers.
        const std::string prefix{"abcd"};
        delegate payload{acpp::connect_arg<&with_payload>, prefix};
        suite.check(payload(6) == 10, "free function with a payload");
    }

    // --- a delegate owns nothing --------------------------------------------
    //
    // The important half of the trade. std::function copies or moves the
    // callable in; a delegate stores a pointer and trusts you. Stating it in a
    // test is the honest way to document a footgun.
    {
        accumulator target;
        acpp::delegate<void(int)> handler{acpp::connect_arg<&accumulator::add>, target};

        suite.check(handler.data() == &target, "the delegate holds a pointer to the instance");

        // Copying the delegate does not copy the target.
        auto copy = handler;
        copy(3);
        suite.check(target.total == 3, "a copied delegate still points at the same object");
        suite.check(copy == handler, "and compares equal to the original");

        // Which means: the caller must guarantee the target outlives the
        // delegate. Nothing here can check that, and that is the point.
        suite.check(std::is_trivially_destructible_v<decltype(handler)>,
                    "a delegate has no destructor, because it owns nothing");
    }

    // --- rebinding ----------------------------------------------------------
    {
        delegate call{acpp::connect_arg<&triple>};
        suite.check(call(2) == 6, "bound");

        call.reset();
        suite.check(!static_cast<bool>(call), "reset disconnects");

        call.connect<&triple>();
        suite.check(call(2) == 6, "and reconnects");

        // The runtime escape hatch: a function pointer with the trampoline
        // signature. No inlining, but it exists for when the target genuinely
        // is not known until run time.
        call.connect([](const void *, int value) -> int { return value + 100; });
        suite.check(call(1) == 101, "a runtime-bound target works too");
    }

    // --- exercise 4: swapping a std::function callback out ------------------
    //
    // A callback table, the shape that actually shows up in driver and event
    // code. The delegate version is half the size per entry and copies with a
    // memcpy; the std::function version has to run a manager per element.
    {
        std::vector<acpp::delegate<void(int)>> fast_table;
        std::vector<std::function<void(int)>> slow_table;

        accumulator a, b;
        fast_table.emplace_back(acpp::connect_arg<&accumulator::add>, a);
        fast_table.emplace_back(acpp::connect_arg<&accumulator::add>, b);
        slow_table.emplace_back([&a](int v) { a.add(v); });
        slow_table.emplace_back([&b](int v) { b.add(v); });

        for(const auto &entry: fast_table) {
            entry(5);
        }

        suite.check(a.total == 5 && b.total == 5, "the delegate table dispatches");

        suite.note("callback table: %zu bytes/entry with delegate, %zu with std::function",
                   sizeof(fast_table[0]), sizeof(slow_table[0]));

        suite.check(sizeof(fast_table[0]) * 2u <= sizeof(slow_table[0]),
                    "a delegate entry is at most half the size of a std::function entry");
    }

    return suite.report();
}
