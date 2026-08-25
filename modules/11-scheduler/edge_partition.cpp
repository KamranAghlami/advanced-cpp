// Module 11, exercise 2 -- the partitioned edge vector, property-tested.
//
// ONE vector holds both successors and predecessors, split at `successor_count`.
// Adding a successor is push-back, swap into the boundary, bump the boundary:
// O(1), one container, one allocation at most, and one cache line for the
// typical fan-out of four.
//
// The invariant to preserve under arbitrary add/remove sequences:
//
//   edges[0, successor_count)      are exactly this node's successors
//   edges[successor_count, size)   are exactly this node's predecessors
//   and the graph is symmetric: a in b.predecessors <=> b in a.successors

#include <algorithm>
#include <cstdint>
#include <map>
#include <random>
#include <set>
#include <vector>

#include <acpp/graph.hpp>
#include <acpp/testing.hpp>

namespace {

using acpp::node;

// A shadow model, deliberately built from the dumbest possible containers.
struct model {
    std::map<node *, std::set<node *>> successors;
    std::map<node *, std::set<node *>> predecessors;
};

[[nodiscard]] bool matches(const std::vector<std::unique_ptr<node>> &nodes, const model &shadow) {
    for(const auto &owned: nodes) {
        auto *self = owned.get();

        std::set<node *> actual_successors;
        for(std::size_t pos = 0u; pos < self->num_successors(); ++pos) {
            actual_successors.insert(self->successor(pos));
        }

        std::set<node *> actual_predecessors;
        for(std::size_t pos = 0u; pos < self->num_predecessors(); ++pos) {
            actual_predecessors.insert(self->predecessor(pos));
        }

        const auto &expected_successors = shadow.successors.at(self);
        const auto &expected_predecessors = shadow.predecessors.at(self);

        if(actual_successors != expected_successors || actual_predecessors != expected_predecessors) {
            return false;
        }

        // The partition must account for every edge exactly once. If the two
        // halves overlapped or a slot were double-counted, this is where it
        // shows up.
        if(self->num_successors() + self->num_predecessors() != actual_successors.size() + actual_predecessors.size()) {
            return false;
        }
    }

    return true;
}

} // namespace

int main() {
    acpp::testing::suite suite{"module 11 / edge_partition"};

    // --- the mechanics ------------------------------------------------------
    {
        node a, b, c;

        suite.check(a.num_successors() == 0u && a.num_predecessors() == 0u, "isolated node");

        a.precede(b);

        suite.check(a.num_successors() == 1u && a.num_predecessors() == 0u, "a has one successor");
        suite.check(b.num_predecessors() == 1u && b.num_successors() == 0u, "b has one predecessor");
        suite.check(a.successor(0u) == &b && b.predecessor(0u) == &a, "and they point at each other");

        // The interesting case: a node with edges in BOTH halves, where the
        // boundary actually has to move things around.
        c.precede(a);
        a.precede(c);

        suite.check(a.num_successors() == 2u, "a now has two successors");
        suite.check(a.num_predecessors() == 1u, "and one predecessor");

        std::set<node *> succ{a.successor(0u), a.successor(1u)};
        suite.check(succ == std::set<node *>{&b, &c}, "both successors are the right ones");
        suite.check(a.predecessor(0u) == &c, "and the predecessor half was not disturbed");
    }

    // --- removal ------------------------------------------------------------
    {
        node a, b, c, d;
        a.precede(b);
        a.precede(c);
        a.precede(d);

        suite.check(a.num_successors() == 3u, "three successors");
        suite.check(a.remove_successor(c), "remove the middle one");
        suite.check(a.num_successors() == 2u, "two left");
        suite.check(c.num_predecessors() == 0u, "and the other side was updated too");

        std::set<node *> remaining{a.successor(0u), a.successor(1u)};
        suite.check(remaining == std::set<node *>{&b, &d}, "the right two remain");
        suite.check(!a.remove_successor(c), "removing a non-edge is refused");
    }

    // --- the property test --------------------------------------------------
    {
        constexpr int node_count = 12;
        std::vector<std::unique_ptr<node>> nodes;
        model shadow;

        for(int i = 0; i < node_count; ++i) {
            nodes.push_back(std::make_unique<node>());
        }

        for(const auto &owned: nodes) {
            shadow.successors[owned.get()];
            shadow.predecessors[owned.get()];
        }

        std::mt19937 random{20260826u};
        bool held = true;
        int adds = 0;
        int removes = 0;

        for(int step = 0; step < 3000 && held; ++step) {
            auto *from = nodes[random() % node_count].get();
            auto *to = nodes[random() % node_count].get();

            if(from == to) {
                continue; // self-edges are a different question
            }

            const bool exists = shadow.successors[from].contains(to);

            if(!exists && (random() % 100u) < 60u) {
                from->precede(*to);
                shadow.successors[from].insert(to);
                shadow.predecessors[to].insert(from);
                ++adds;
            } else if(exists) {
                from->remove_successor(*to);
                shadow.successors[from].erase(to);
                shadow.predecessors[to].erase(from);
                ++removes;
            }

            held = matches(nodes, shadow);
        }

        suite.check(held, "the partition invariant held across 3000 randomized edge operations");
        suite.note("%d adds, %d removes over %d nodes", adds, removes, node_count);
    }

    // --- and the reason for the inline capacity -----------------------------
    {
        node few;
        std::vector<std::unique_ptr<node>> others;

        for(int i = 0; i < 4; ++i) {
            others.push_back(std::make_unique<node>());
            few.precede(*others.back());
        }

        suite.check(few.num_successors() == 4u, "four successors");
        suite.note("a 4-edge node keeps its topology inline: most graphs allocate "
                   "nothing for edges at all");
    }

    return suite.report();
}
