// Module 11, exercise 4 -- an exception from a worker thread, and the decision
// the course says has no universally right answer.
//
// THE DECISION, stated up front so it is a decision and not an accident:
//
//   When several tasks throw simultaneously, the FIRST captured exception is
//   rethrown from wait(); the rest are counted and discarded.
//
// Why not the alternatives:
//
//   * "throw them all" is not expressible -- there is no std::exception_ptr
//     that means "these three", and C++20 has no aggregate exception type.
//     std::nested_exception chains a cause, not siblings.
//   * "throw the last" is arbitrary in a way "first" is not: the first is the
//     one whose failure most plausibly caused the rest, because the graph is
//     cancelled the instant it lands.
//   * "collect them into a vector and throw that" moves the problem to the
//     caller, who now has to handle a type they did not throw. Reasonable for a
//     library that owns its error type; wrong for one that propagates the
//     user's.
//
// So: first wins, count the rest, and expose the count so a caller who cares
// can tell "one task failed" from "the graph collapsed".

#include <atomic>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <acpp/executor.hpp>
#include <acpp/testing.hpp>

namespace {

struct task_error: std::runtime_error {
    explicit task_error(const std::string &what)
        : std::runtime_error{what} {}
};

} // namespace

int main() {
    acpp::testing::suite suite{"module 11 / exception_propagation"};

    acpp::executor pool{4u};

    // --- one task throws ----------------------------------------------------
    {
        acpp::taskflow graph;
        std::atomic<int> ran{0};

        auto a = graph.emplace([&] { ran.fetch_add(1); });
        auto b = graph.emplace([] { throw task_error{"b failed"}; });
        auto c = graph.emplace([&] { ran.fetch_add(1); });

        a.precede(b);
        b.precede(c);

        auto run = pool.run(graph);

        bool caught = false;
        std::string message;

        try {
            run->wait();
        } catch(const task_error &error) {
            caught = true;
            message = error.what();
        }

        suite.check(caught, "the exception reached the waiting thread");
        suite.check(message == "b failed", "and it is the original exception, not a copy of a description");
        suite.check(run->cancelled(), "the graph was cancelled");
        suite.check(ran.load() == 1, "the successor of the throwing task did not run");
    }

    // --- wait() is not re-armed ---------------------------------------------
    //
    // Rethrowing the same exception on every subsequent wait() would make a
    // second wait look like a second failure. It is consumed.
    {
        acpp::taskflow graph;
        graph.emplace([] { throw task_error{"once"}; });

        auto run = pool.run(graph);

        int throws = 0;
        for(int attempt = 0; attempt < 2; ++attempt) {
            try {
                run->wait();
            } catch(const task_error &) {
                ++throws;
            }
        }

        suite.check(throws == 1, "wait() rethrows once, then reports success");
    }

    // --- several tasks throw at once ----------------------------------------
    {
        acpp::taskflow graph;
        constexpr int throwers = 8;

        auto root = graph.emplace([] {});

        for(int i = 0; i < throwers; ++i) {
            auto bad = graph.emplace([i] { throw task_error{"thrower " + std::to_string(i)}; });
            root.precede(bad);
        }

        auto run = pool.run(graph);

        bool caught = false;
        std::string message;

        try {
            run->wait();
        } catch(const task_error &error) {
            caught = true;
            message = error.what();
        }

        suite.check(caught, "one of them propagated");
        suite.check(message.starts_with("thrower "), "and it is one of the real exceptions");

        // The count is the part the decision above makes visible. Cancellation
        // races the other throwers, so the number that actually ran and threw
        // is not deterministic -- which is exactly why it is reported rather
        // than asserted to a fixed value.
        suite.note("%zu of %d throwers reported before cancellation took effect",
                   run->exception_count(), throwers);
        suite.check(run->exception_count() >= 1u, "at least one exception was recorded");
        suite.check(run->exception_count() <= throwers, "and no more than were possible");
    }

    // --- cancellation still drains ------------------------------------------
    //
    // The failure mode this guards against: a cancelled graph that never
    // finishes, because the cancelled nodes stopped decrementing the counter
    // that wait() is blocked on. Cancellation must skip the *work*, not the
    // bookkeeping.
    {
        acpp::taskflow graph;
        constexpr int width = 32;

        auto root = graph.emplace([] { throw task_error{"immediately"}; });
        auto join = graph.emplace([] {});

        for(int i = 0; i < width; ++i) {
            auto leaf = graph.emplace([] { std::this_thread::yield(); });
            root.precede(leaf);
            leaf.precede(join);
        }

        auto run = pool.run(graph);

        bool caught = false;
        try {
            run->wait(); // must return, not hang
        } catch(const task_error &) {
            caught = true;
        }

        suite.check(caught, "a graph cancelled at its root still completes and rethrows");
    }

    // --- the pool survives --------------------------------------------------
    //
    // An exception must not take a worker thread with it. If the try/catch
    // around the invoke loop were missing, the throw would unwind a worker out
    // of the pool and every later run would be slower or would hang.
    {
        acpp::taskflow graph;
        std::atomic<int> total{0};

        for(int i = 0; i < 32; ++i) {
            graph.emplace([&] { total.fetch_add(1); });
        }

        pool.run(graph)->wait();
        suite.check(total.load() == 32, "the pool still works after the exceptions above");
        suite.check(pool.num_workers() == 4u, "with all four workers alive");
    }

    return suite.report();
}
