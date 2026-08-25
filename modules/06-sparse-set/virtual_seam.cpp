// Module 6, exercise 4 -- the private-virtual seam, and a derived payload class.
//
// The base is type-erased over entities only. The derived class knows about a
// payload and keeps a parallel array in sync. The base reorders entities; the
// derived mirrors the reorder -- *without the base ever knowing the payload
// type*. Non-Virtual Interface: the base owns the algorithm, the derived owns
// one step of it.
//
// This is deliberately a minimal payload class. The real one -- paged, for
// pointer stability, with backward iteration -- is Module 7's job.

#include <cstdint>
#include <string>
#include <vector>

#include <acpp/sparse_set.hpp>
#include <acpp/testing.hpp>

namespace {

enum class entity : std::uint32_t {};

using base_type = acpp::basic_sparse_set<entity>;
using traits = acpp::handle_traits<entity>;

[[nodiscard]] entity make(const std::uint32_t index) noexcept {
    return traits::construct(index, 0u);
}

template<typename Type>
class flat_storage: public base_type {
public:
    using base_type::base_type;

    template<typename... Args>
    Type &emplace(const entity entt, Args &&...args) {
        const auto pos = push(entt);
        // in_place reuses a hole, so the payload slot may already exist.
        if(pos >= payload.size()) {
            payload.resize(pos + 1u);
        }
        payload[pos] = Type{std::forward<Args>(args)...};
        return payload[pos];
    }

    [[nodiscard]] Type &get(const entity entt) { return payload[index(entt)]; }
    [[nodiscard]] const Type &get(const entity entt) const { return payload[index(entt)]; }

    [[nodiscard]] std::size_t destructions() const noexcept { return destroyed; }
    [[nodiscard]] std::size_t moves() const noexcept { return moved; }

private:
    // The three hooks the base calls. Private and virtual: no caller reaches
    // them, and the base must.
    const void *get_at(const std::size_t pos) const override {
        return pos < payload.size() ? &payload[pos] : nullptr;
    }

    void swap_or_move(const std::size_t lhs, const std::size_t rhs) override {
        if(lhs < payload.size() && rhs < payload.size()) {
            std::swap(payload[lhs], payload[rhs]);
            ++moved;
        }
    }

    void move_into(const std::size_t from, const std::size_t to) override {
        if(from < payload.size() && to < payload.size()) {
            payload[to] = std::move(payload[from]);
            ++moved;
        }
    }

    void destroy_at(const std::size_t pos) override {
        if(pos < payload.size()) {
            payload[pos] = Type{};
            ++destroyed;
        }
    }

    std::vector<Type> payload;
    std::size_t destroyed{};
    std::size_t moved{};
};

// The exercise's static_assert: the base really has no knowledge of Type.
// Two independent instantiations of the derived template share one base type,
// which is what makes a heterogeneous container of `base_type *` possible.
static_assert(std::is_same_v<flat_storage<int>::value_type, flat_storage<std::string>::value_type>);
static_assert(std::is_base_of_v<base_type, flat_storage<int>>);
static_assert(std::is_base_of_v<base_type, flat_storage<std::string>>);
static_assert(sizeof(base_type) < sizeof(flat_storage<std::string>),
              "the payload lives entirely in the derived class");

} // namespace

int main() {
    acpp::testing::suite suite{"module 06 / virtual_seam"};

    // --- swap_and_pop: the base moves an entity, the derived follows ---------
    {
        flat_storage<std::string> storage;

        for(std::uint32_t i = 0u; i < 5u; ++i) {
            storage.emplace(make(i), "value-" + std::to_string(i));
        }

        suite.check(storage.get(make(4u)) == "value-4", "payload is reachable through the entity");

        // Erasing element 1 moves element 4 into position 1. The base does the
        // entity half; swap_or_move does the payload half.
        storage.erase(make(1u));

        suite.check(storage.size() == 4u, "the entity array shrank");
        suite.check(storage.index(make(4u)) == 1u, "element 4 moved to position 1");
        suite.check(storage.get(make(4u)) == "value-4",
                    "and its payload came with it -- the seam did its job");
        suite.check(storage.get(make(0u)) == "value-0", "untouched elements are untouched");
        suite.check(storage.destructions() == 1u, "the erased payload was destroyed once");
        suite.check(storage.moves() == 1u, "and exactly one payload move happened");
    }

    // --- the base can operate on a payload it cannot name --------------------
    //
    // This is what the type erasure buys: one loop over heterogeneous storages.
    {
        flat_storage<int> integers;
        flat_storage<std::string> strings;

        for(std::uint32_t i = 0u; i < 4u; ++i) {
            integers.emplace(make(i), static_cast<int>(i * 10));
            strings.emplace(make(i), std::to_string(i));
        }

        std::vector<base_type *> pools{&integers, &strings};

        std::size_t total = 0u;
        for(auto *pool: pools) {
            // Only entity-level operations here. The base has no idea one of
            // these holds ints and the other std::strings.
            pool->erase(make(2u));
            total += pool->count();
        }

        suite.check(total == 6u, "the base erased from both pools knowing nothing about either");
        suite.check(integers.get(make(3u)) == 30, "int payload survived");
        suite.check(strings.get(make(3u)) == "3", "string payload survived");
        suite.check(!integers.contains(make(2u)) && !strings.contains(make(2u)), "both dropped the entity");
    }

    // --- in_place: nothing moves, so the seam should see no moves ------------
    {
        flat_storage<std::string> storage{acpp::deletion_policy::in_place};

        for(std::uint32_t i = 0u; i < 5u; ++i) {
            storage.emplace(make(i), "value-" + std::to_string(i));
        }

        storage.erase(make(1u));

        suite.check(storage.moves() == 0u, "in_place never moves a payload -- that is its whole point");
        suite.check(storage.destructions() == 1u, "it only destroys");
        suite.check(storage.index(make(4u)) == 4u, "and every survivor keeps its position");

        storage.emplace(make(9u), "reused");
        suite.check(storage.index(make(9u)) == 1u, "the hole was reused");
        suite.check(storage.get(make(9u)) == "reused", "with a fresh payload in the old slot");
    }

    return suite.report();
}
