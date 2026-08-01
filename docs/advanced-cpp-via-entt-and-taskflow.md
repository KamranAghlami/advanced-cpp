# Advanced C++ by Dissection: EnTT + Taskflow

A 12-module course built from the actual source of two header-only libraries. Every module points at real symbols in real files, explains the technique behind them, and ends with something you build yourself.

**Pinned versions** (what this course was written against — check out these exact commits so file/line references match):

```bash
git clone https://github.com/skypjack/entt.git && git -C entt checkout 85c6bba
git clone https://github.com/taskflow/taskflow.git && git -C taskflow checkout c4da2a4
```

EnTT's `version.h` reads 4.0.0, but note that's master *in development toward* v4, not a tagged release — hence the commit pin rather than a tag. Taskflow is at `TF_VERSION 400100` → 4.1.0. Both require **C++20** (EnTT declares `cxx_std_20`; Taskflow sets `CMAKE_CXX_STANDARD 20` and uses `std::bit_ceil`, `atomic_flag::test`, etc.). Both are MIT. Both are ~20–25 kLOC of headers, which is small enough to actually finish.

**Validation status.** Every quoted code fragment, symbol name, file path, mask value, and build flag in this document was checked against the pinned commits during review — including one round that caught the document describing a commented-out duplicate of `_explore_task` rather than the live one. Claims deliberately left for *you* to verify (codegen questions, benchmark predictions) are marked as such in the text; treat everything phrased as "check/verify/prove this yourself" as an exercise, not an oversight.

**Why these two together:** EnTT is a masterclass in *compile-time machinery and memory layout*. Taskflow is a masterclass in *lock-free concurrency and scheduler design*. Almost nothing overlaps. Between them you cover the two halves of modern systems C++ that most engineers only half-know.

---

## Table of contents

| # | Module | Primary source | Core skill |
|---|--------|----------------|------------|
| 0 | Tooling setup | — | Instrumenting your own learning |
| 1 | Type identity without RTTI | `core/type_info.hpp`, `core/hashed_string.hpp` | consteval, overload ranking, ODR across TUs |
| 2 | Traits as an API surface | `core/type_traits.hpp`, `entity/component.hpp` | Constrained partial specialization, policy inference |
| 3 | Layout economy | `core/compressed_pair.hpp` | EBO, allocator-aware design, empty types |
| 4 | The freestanding shim | `src/entt/stl/` | Portability without `#ifdef` soup |
| 5 | Bit-packed handles | `entity/entity.hpp` | Generational indices, tombstones |
| 6 | The sparse set | `entity/sparse_set.hpp` | Paged indirection, intrusive free lists, deletion policies |
| 7 | Storage & view iteration | `entity/storage.hpp`, `entity/view.hpp` | Cache-conscious iteration, iterator composition |
| 8 | Type erasure, three ways | `core/any.hpp`, `signal/delegate.hpp`, `poly/poly.hpp` | SBO, manual vtables, codegen cost |
| 9 | Chase–Lev work-stealing deque | `core/wsq.hpp` | Memory ordering you can defend |
| 10 | Sleeping without lost wakeups | `core/nonblocking_notifier.hpp`, `core/executor.hpp` | Two-phase commit, packed atomic state |
| 11 | Scheduler & graph representation | `core/graph.hpp`, `core/executor.hpp` | Variant nodes, join counters, lifetime |
| 12 | Parallel algorithm design | `algorithm/partitioner.hpp`, `algorithm/pipeline.hpp` | Policy objects, `corun`, pipeline scheduling |
| ★ | Capstone | — | Ship something |

Suggested pace: one module per week, ~4–6 focused hours each. Modules 9 and 10 are worth two weeks.

---

## Module 0 — Set up your instruments first

You cannot learn from a codebase you can only read. Get these working before Module 1.

**Local build of both libraries with tests:**

```bash
# EnTT
cmake -S entt -B entt/build -DENTT_BUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build entt/build -j

# Taskflow
cmake -S taskflow -B taskflow/build -DTF_BUILD_TESTS=ON -DTF_BUILD_EXAMPLES=ON
cmake --build taskflow/build -j
```

**A scratch TU you can rebuild in under two seconds.** Header-only libraries punish sloppy iteration loops. Make a single `scratch.cpp` and a one-line rebuild command; you will run it several hundred times over this course.

**The instrument list:**

| Tool | What you use it for | Invocation |
|---|---|---|
| Compiler Explorer | Confirming a technique costs zero instructions | godbolt.org, `-O2 -std=c++20` |
| `cppinsights.io` | Seeing what templates/lambdas/CTAD actually expand to | paste and read |
| `-ftime-trace` | Finding which header is eating your compile time | clang; view in `chrome://tracing` |
| `perf stat` / `perf record` | Cache misses, branch misses, IPC | `perf stat -e cache-misses,instructions ./bench` |
| `valgrind --tool=cachegrind` | Deterministic cache modelling (great in WSL2 where PMU access is flaky) | `cachegrind --D1=32768,8,64` |
| TSan | Module 9–11 only. Non-negotiable there. | `-fsanitize=thread -g` |
| `nm -C --size-sort` | Proving type erasure did or did not emit what you thought | on your object files |
| Google Benchmark | Anything you claim is faster | link into scratch project |

**How to read a header-only library (the method you'll use all course):**

1. Read `fwd.hpp` first. It's the author's own summary of the type inventory.
2. Read the `internal::` namespace before the public class. The public class is usually thin.
3. For any class, read in this order: private data members → private helpers → constructors → the two or three methods that actually do work. Skip the accessor wall.
4. When you hit a trick you don't understand, isolate it in a 15-line file on Compiler Explorer before continuing. Do not read on hoping it'll become clear.
5. Keep a `notes.md` with one entry per technique: *name, file, one-sentence mechanism, where I'd use it.*

---

# Phase A — Compile-time machinery

## Module 1 — Type identity without RTTI

**Read:** `src/entt/core/type_info.hpp`, then `src/entt/core/hashed_string.hpp`, then `src/entt/config/config.h` (the `ENTT_PRETTY_FUNCTION` block).

This file is ~230 lines and contains four distinct techniques. Take it slowly.

### 1.1 Stringifying a type without `typeid`

`internal::pretty_function<Type>()` returns the compiler's `__PRETTY_FUNCTION__` / `__FUNCSIG__`, which embeds the template argument's spelling. `stripped_type_name<Type>()` then slices that with `string_view::find_first_of` against compiler-specific prefix/suffix markers.

The insight worth stealing: **the compiler already knows the type name at compile time; you just need to extract it from a string it was going to emit anyway.** No RTTI, no `-frtti`, works on the freestanding toolchains where `typeid` is unavailable or bloats the binary.

### 1.2 The `int`/`char` overload-ranking trick

This pattern appears twice:

```cpp
template<typename Type, auto = stripped_type_name<Type>().find_first_of('.')>
[[nodiscard]] ENTT_CONSTEVAL string_view type_name(int) noexcept;   // preferred

template<typename Type>
[[nodiscard]] string_view type_name(char) noexcept;                 // fallback
```

Two things are happening simultaneously:

- The **non-type template parameter with a default** forces `stripped_type_name<Type>()` to be evaluated in a constant expression during template argument deduction. If it can't be (some compilers produce names that break the extraction for local types, lambdas, etc.), that's a *substitution failure*, not an error, and the overload silently drops out.
- The **`int` vs `char` parameter** gives a deterministic preference order when both are viable: the call site passes `0`, which is an exact match for `int` and a conversion for `char`.

Result: types whose names can be computed at compile time get a `consteval` path with zero runtime cost; everything else falls back to a function-local `static` computed once. Two different implementations, one call site, chosen by the compiler.

**This is a general technique.** You will reuse it any time you want "constexpr if possible, runtime if not" without `if constexpr` (which can't help you here, because the *failure* is in the constant evaluation itself).

### 1.3 Sequential IDs and the ODR trap

`internal::type_index::next()` hands out a monotonically increasing counter, and `type_index<Type>::value()` caches one per type in a function-local `static`. Simple. But look at what surrounds it: `ENTT_API` on the struct, and `ENTT_MAYBE_ATOMIC` on the counter.

Those two macros exist because of a real, nasty failure mode: **a function-local static inside a template inside a header can be instantiated separately in each shared object**, giving you two different IDs for the same type across a DLL boundary — and a registry that silently loses components. `ENTT_API` controls visibility so the linker merges them; `ENTT_MAYBE_ATOMIC` handles concurrent first-touch.

Read `config/config.h` and understand what `ENTT_API` expands to on each platform. This bug class shows up in any plugin architecture.

### 1.4 The compile-time hash

`hashed_string` is an FNV-1a over the name, computable at compile time via `hashed_string::value(ptr, len)`. Note it also gives you `operator""_hs`, so string literals become integer IDs at compile time — a technique directly applicable to command dispatch, config keys, and protocol tags in constrained firmware.

> **Watch for:** the hash is *not* collision-free, and the library is explicit about this. Think about what a collision costs in your own design before you copy the pattern.

### Exercises

1. **(45 min)** Reproduce `stripped_type_name` from scratch for GCC, Clang, and MSVC. Verify on Compiler Explorer that `type_name<std::vector<int>>()` produces a `string_view` into a static string with *zero* runtime instructions at `-O2`.
2. **(1 hr)** Build a `constexpr` string→ID mapper: `enum class cmd : uint32_t { ... }` where each value is `"POWER_ON"_hs`. Write a `switch` over hashed literals. Inspect the codegen — confirm it's a jump table or a compare chain over immediates, not a string comparison.
3. **(1.5 hr)** Write a two-TU + one-shared-object test that demonstrates the ODR/visibility failure with `type_index`, then fix it with visibility attributes. This is the exercise most people skip and most people later need.
4. **(30 min)** Adapt the `int`/`char` ranking trick to a different problem: a `to_string(T)` that prefers a `constexpr` path when `T` is a literal type and falls back otherwise.

**Checkpoint:** you can explain, without notes, why `type_name<T>(0)` picks one overload over the other, and what breaks if you remove the `auto =` template parameter.

---

## Module 2 — Traits as an API surface

**Read:** `src/entt/entity/component.hpp` (only 59 lines — read every one), then skim `src/entt/core/type_traits.hpp`.

`component.hpp` is the smallest file in the library with the biggest design lesson.

### 2.1 Behaviour inferred from type properties

```cpp
template<typename Type>
struct in_place_delete
  : bool_constant<!(is_move_constructible_v<Type> && is_move_assignable_v<Type>)> {};
```

Read that carefully. The library is deciding a **storage strategy** — whether deletion swaps-and-pops (invalidating pointers) or leaves a tombstone in place (preserving them) — by asking whether the type can be moved at all. A non-movable component *cannot* be swap-and-popped, so the correct policy is derived, not configured.

Same file:

```cpp
template<typename Type>
struct page_size
  : integral_constant<size_t, !is_empty_v<ENTT_ETO_TYPE(Type)> * ENTT_PACKED_PAGE> {};
```

An empty type gets `page_size == 0`, which downstream in `storage.hpp` means *allocate no payload at all*. A tag component costs one entity ID and nothing else. The multiplication-by-a-bool is a small idiom worth noting: it collapses a branch into arithmetic at compile time.

### 2.2 Constrained partial specialization replaces the detection idiom

```cpp
template<typename Type>
requires Type::in_place_delete
struct in_place_delete<Type>: true_type {};

template<typename Type>
requires convertible_to<decltype(Type::page_size), size_t>
struct page_size<Type>: integral_constant<size_t, Type::page_size> {};
```

In C++17 this would have been `void_t` detection plus a two-layer `std::conditional`. In C++20 it's three lines and reads like English. The user opts in by declaring `static constexpr bool in_place_delete = true;` inside their own type — no macro, no specialization of a library template in the library's namespace.

**Design principle to extract:** offer *sane defaults derived from the type*, an *inline opt-in* via a member, and a *full override* via specializing `component_traits`. Three levels of customization, each more invasive than the last. This is the customization-point ladder, and it's what separates a library from a framework.

### 2.3 The trait vocabulary

Skim `core/type_traits.hpp` for these and note the ones you don't have in your own toolbox:

- `type_list` / `type_list_cat` / `type_list_unique` / `type_list_contains` — compile-time type sequence algebra without tuples.
- `is_ebco_eligible_v` — "is this class empty, non-final, and thus inheritable for layout compression?" (Module 3 uses it.)
- `constness_as_t` — propagates const-ness from one type to another. Appears all over the iterator code.
- `member_class_t` — recovers `C` from `R (C::*)(Args...)`.

### Exercises

1. **(1 hr)** Write your own `serialization_traits<T>` with the three-level ladder: default inferred from `is_trivially_copyable_v`, opt-in via a `static constexpr` member, override via specialization. Prove all three paths work.
2. **(1 hr)** Implement `type_list_unique` yourself, then compare against EnTT's. Measure instantiation depth with `-ftime-trace` for a 64-type list. Try a fold-expression version and a recursive version; they will not cost the same.
3. **(30 min)** Take one policy currently controlled by a macro in code you own and convert it to a trait with an inferred default.

**Checkpoint:** you can articulate why `in_place_delete` defaulting to "the inverse of movability" is a correctness decision rather than a performance one.

---

## Module 3 — Layout economy

**Read:** `src/entt/core/compressed_pair.hpp`.

### 3.1 EBO, done properly

The pattern is a primary template that stores by value, and a `requires is_ebco_eligible_v<Type>` partial specialization that *inherits* instead:

```cpp
template<typename Type, size_t>
struct compressed_pair_element { /* Type value; */ };

template<typename Type, size_t Tag>
requires is_ebco_eligible_v<Type>
struct compressed_pair_element<Type, Tag>: Type { /* nothing */ };
```

The `size_t Tag` parameter is essential and easy to miss: without it, a `compressed_pair<Empty, Empty>` would inherit from the same base twice, which is ill-formed. Tagging with `0u` and `1u` makes them distinct types.

Also note `get()` returns `*this` in the EBO specialization and `value` in the primary — the two implementations expose an identical interface, which is what lets `compressed_pair` itself be written once.

### 3.2 Why this matters far beyond `std::pair`

Every allocator-aware container in the library stores its allocator this way. `std::allocator<T>` is empty. So is a stateless deleter, a stateless comparator, a stateless hash. Without EBO, a `dense_map` would pay 8–24 bytes per instance for four empty objects. With it, zero.

In C++20 you can often reach for `[[no_unique_address]]` instead, which is less code. Read the file, then write both versions and compare `sizeof` across compilers — MSVC's handling of `[[no_unique_address]]` will surprise you, and that's exactly why EnTT still ships the inheritance version.

### 3.3 Where empty types disappear entirely

Cross-reference `component_traits::page_size == 0` from Module 2 with how `basic_storage` uses it. A tag component has *no payload array*. Then look at how `get_as_tuple()` returns an empty tuple for such types, so that `view.each()` still composes correctly. The abstraction survives the optimization — that's the hard part.

### Exercises

1. **(45 min)** Build `compressed_pair` from scratch. Then build a `[[no_unique_address]]` version. Compare `sizeof(pair<empty, int>)` on GCC, Clang, and MSVC (use Compiler Explorer for all three).
2. **(1 hr)** Take a container you've written — a ring buffer, a pool, anything — and make it allocator-aware with EBO'd allocator storage. Confirm `sizeof` is unchanged for the stateless-allocator case.
3. **(1 hr)** Write a `static_assert` battery that documents your layout assumptions (`sizeof`, `alignof`, `offsetof`, `is_standard_layout_v`). This is the habit that catches ABI drift when the toolchain changes underneath you.

**Checkpoint:** you can explain the `Tag` parameter's purpose to someone else and give the exact code that breaks without it.

---

## Module 4 — The freestanding shim

**Read:** `src/entt/stl/` — start with `type_traits.hpp` and `vector.hpp`, then look at the pattern across the directory.

Every file follows the same shape:

```cpp
#if __has_include(<entt/ext/stl/type_traits.hpp>)
#    include <entt/ext/stl/type_traits.hpp>
#else
#    include <type_traits>
namespace entt::stl {
    using std::conditional_t;
    using std::decay_t;
    // ... an explicit, enumerated re-export
}
#endif
```

Then the entire library uses `stl::` instead of `std::` internally.

### Why this is the most directly applicable module in the course

If you have ever shipped C++ onto a target where the vendor's libstdc++ was ancient, or `std::vector` pulled in exception machinery you couldn't afford, or the toolchain had a half-implemented `<memory>` — this is the clean answer. It gives you:

- **A single seam.** One directory to swap when you port. No `#ifdef` scattered through 20 kLOC.
- **An explicit dependency manifest.** The `using` list *is* the complete inventory of what the library needs from the standard library. That's an auditable document. Count it — you'll find it's far smaller than you'd guess.
- **User-supplied replacement without forking.** Drop your own `entt/ext/stl/vector.hpp` on the include path and `__has_include` picks it up. No build flags, no patches to maintain across upstream bumps.

Contrast this with the alternatives you've probably lived with: forking the library, `-D` macro walls, or wrapping everything in your own namespace by hand.

### Exercises

1. **(30 min)** Enumerate the full set of `std::` names EnTT depends on by grepping the `stl/` directory. Write down the count. Is it what you expected?
2. **(2 hr)** Build a minimal `ext/stl/vector.hpp` that forwards to a fixed-capacity, no-heap vector of your own. Get *some* subset of EnTT compiling against it. You will not get all of it, and finding out precisely where it breaks is the point of the exercise.
3. **(1 hr)** Apply the seam pattern to one module in code you own. Even if you never swap the implementation, the dependency manifest has standalone value in review.

**Checkpoint:** you can explain to a teammate why `__has_include` beats a build-system flag for this job.

---

# Phase B — Data structure design

## Module 5 — Bit-packed handles

**Read:** `src/entt/entity/entity.hpp`, particularly `internal::entt_traits` and `basic_entt_traits`.

### 5.1 The layout

For a 32-bit entity:

```cpp
static constexpr entity_type entity_mask  = 0xFFFFF;   // 20 bits → 1,048,575 live entities
static constexpr entity_type version_mask = 0xFFF;     // 12 bits → 4,096 reuses before wrap
```

One `uint32_t` carries both an index and a generation counter. `to_entity()` masks off the index, `to_version()` shifts and masks the generation, `combine()` reassembles.

### 5.2 Why generational indices are the right answer

The problem: you want stable, cheap, copyable handles into a dense array. Raw indices dangle after a slot is reused; pointers dangle after reallocation; `shared_ptr` costs you an atomic refcount and a cache miss per dereference.

Generational indices solve it by making staleness *detectable*: bump the version on release, and any handle holding the old version fails validation. Cost: one integer compare. This pattern is everywhere in systems code — file descriptors with generation counts, slab allocators, GPU resource handles, and it's the correct answer for peripheral/session handles in device firmware too.

### 5.3 The details that make it real

- `null` and `tombstone` are constants with special bit patterns and conversion operators, not sentinel `-1` magic numbers. Read how `null_t::operator==` is defined — it deliberately compares only the entity part in some contexts.
- `basic_entt_traits::length` uses `std::popcount(entity_mask)` to derive the shift, so changing the mask is a one-line edit.
- The `static_assert` inside `to_entity` verifies the mask is `2^n - 1`. Constraints on your own constants, checked at compile time.
- Note the C++20 `requires requires` blocks that let a user's enum or wrapper type inherit traits from its underlying type. That's how you make `enum class my_entity : uint32_t {}` work as a first-class entity type.

### Exercises

1. **(2 hr)** Implement a `handle_allocator<T, IndexBits, VersionBits>`: `allocate()` returns a packed handle, `release(h)` bumps the version and pushes the index onto a free list, `is_valid(h)` checks. Make the bit split a template parameter with `static_assert`s.
2. **(45 min)** Handle version wraparound explicitly. What *should* happen at 4,096 reuses of a slot? Read what EnTT does. Decide what your allocator does and document it. (There is no universally right answer — the point is that it must be a decision, not an accident.)
3. **(1 hr)** Write the property test: allocate/release/validate in randomized sequences, assert that no stale handle ever validates. Run it under a fuzzer if you have one wired up.

**Checkpoint:** given a 64-bit handle budget and a requirement of "at most 10M live objects, must detect staleness for at least a year of churn at 1000 releases/sec," you can pick the bit split and justify it.

---

## Module 6 — The sparse set

**Read:** `src/entt/entity/sparse_set.hpp`. This is the heart of the library. Budget real time.

### 6.1 The core structure

Two arrays. `packed` is a contiguous, dense array of entity IDs — iterate this, it's cache-friendly. `sparse` is indexed by entity index and stores the position into `packed` — this gives O(1) lookup. The invariant is `packed[sparse[e]] == e`.

Classic. What makes this implementation worth studying is everything layered on top.

### 6.2 Paged sparse array

`sparse` isn't one big array; it's a vector of fixed-size pages, allocated on demand:

```cpp
pos_to_page(pos)  →  pos / traits_type::page_size
fast_mod(pos, page_size)  →  pos & (page_size - 1u)
```

Why: the sparse array is indexed by *entity ID*, which can be sparse. A flat array sized to the largest ID wastes memory catastrophically when IDs are spread out. Paging bounds the waste to one page.

`fast_mod` (in `core/bit.hpp`) is three lines and carries a `has_single_bit` assertion — the whole scheme depends on power-of-two page sizes, and the code says so at compile time.

Look at `assure_at_least()`: it grows the page vector, allocates a page through `allocator_traits`, and `uninitialized_fill`s it with `null`. Allocator-aware, exception-shaped, no `new`.

### 6.3 Three deletion policies — the real lesson

This is the part to study hardest. The same container supports three removal semantics, and each exists for a concrete reason:

**`swap_and_pop`** — move the last element into the hole, shrink. Keeps the packed array dense with no holes. Invalidates the position of the moved-from element (so pointers into it dangle). Default.

**`in_place`** — leave a hole and thread it into a free list. Look closely:

```cpp
void in_place_pop(const Entity entt) {
    const auto pos = entity_to_pos(exchange(sparse_ref(entt), null));
    packed[pos] = traits_type::combine(
        static_cast<traits_type::entity_type>(exchange(head, pos)), tombstone);
}
```

The freed slot in `packed` now stores *the index of the previously freed slot* in its entity bits, and `tombstone` in its version bits. `head` points at the most recent hole. **The free list lives inside the array it manages, at zero extra memory cost**, and the tombstone version makes holes distinguishable from live entries during iteration. This is the single most elegant thing in the file.

**`swap_only`** — partitions the array at `head`: live elements below, dead above. Nothing is destroyed; entries move across the boundary. The flagship user is the registry's **entity storage itself**: look at `storage.hpp` around the `basic_storage<Entity, Entity>` specialization, where `storage_policy = deletion_policy::swap_only`. Released entities sit above `head` with bumped versions — which means **entity recycling needs no separate free-list structure at all**; the partition *is* the free list, and creating an entity is just moving the boundary back down. Connect this to Module 5's generational indices and the whole entity lifecycle clicks into place.

Note `policy_to_head()` — a branchless way to initialize `head` differently per policy, written as a multiplication by a bool rather than a ternary. The comment above it explains it could be `auto` but GCC emits a false-positive warning. Small honesty in the source that tells you the author tested widely.

### 6.4 The virtual seam

`get_at()` and `swap_or_move()` are `private virtual`. The base is `basic_sparse_set` (type-erased over entities only); the derived `basic_storage<T>` knows about payload. The base can therefore reorder entities and the derived class keeps the parallel payload array in sync — **without the base knowing the payload type**.

Private-virtual with a non-virtual public interface is the Non-Virtual Interface idiom, and this is a textbook use of it: the base owns the algorithm, the derived owns one step of it.

### Exercises

1. **(3 hr)** Implement a sparse set with paged sparse array and `swap_and_pop`. Test the invariant `packed[sparse[e]] == e` after randomized insert/erase sequences.
2. **(2 hr)** Add `in_place` deletion with the intrusive free list. This is harder than it looks — the tombstone encoding and the `head` update must be exactly right. Write the test that inserts, erases every third element, then reinserts and checks that holes were reused in LIFO order.
3. **(1 hr)** Benchmark iteration over your packed array vs. a `std::unordered_map<uint32_t, T>` with the same contents at 10K/100K/1M elements. Use `perf stat -e cache-misses` or cachegrind. Write down the ratio; it should be dramatic and you should be able to explain it in terms of cache lines, not vibes.
4. **(1 hr)** Add the private-virtual seam and a derived payload class. Verify with a `static_assert` that the base has no knowledge of `T`.

**Checkpoint:** you can draw the memory layout of a sparse set with three holes on a whiteboard, including exactly what's in each tombstoned `packed` slot.

---

## Module 7 — Storage and view iteration

**Read:** `src/entt/entity/storage.hpp` (the `basic_storage` data members and `element_at` / `assure_at_least`), then `src/entt/entity/view.hpp` (`view_iterator`, `basic_common_view::refresh`, `extended_view_iterator`).

### 7.1 Paged payload

```cpp
auto &element_at(const size_t pos) const {
    return payload[pos / traits_type::page_size][fast_mod(pos, traits_type::page_size)];
}
```

Payload is *also* paged — but for a different reason than the sparse array. Here it's about **pointer stability**: with `in_place` deletion and paged payload, a component's address never changes for its whole lifetime, because pages are never reallocated. A flat `std::vector<T>` would invalidate everything on growth.

The trade: one extra indirection per access, and iteration must be page-aware. Read `storage_iterator` carefully — two things are going on:

- Dereference *does* compute `pos / Page` and `fast_mod(pos, Page)` on every access. That's fine because `Page` is a compile-time power-of-two constant, so both compile to a shift and a mask. Verify this yourself on Compiler Explorer; it's a good calibration of when "division per element" is and isn't a cost.
- The iterator runs **backward** — the comments say "intentionally reversed due to backward iteration," `index()` returns `offset - 1`, and `operator-` and `operator<=>` are both flipped. Work out the consequences carefully, because two different guarantees fall out of this one choice. `begin()` sits at the *high* end of the packed array and `++` walks toward index 0. So when you erase the current element under swap-and-pop, the element swapped into the hole comes from the highest live index — a slot you have **already passed** — and nothing gets skipped. Conversely, elements *created* mid-iteration are appended at high packed indices, which are behind `begin()`, so they are **not** visited at all. Safe erasure and non-visitation of new elements are the same design decision seen from two directions. Prove both to yourself with a test before you trust either.

### 7.2 Choosing the leading storage

A multi-component view iterates one storage and filters against the rest. Which one to iterate matters enormously — pick the 1M-element pool over the 12-element pool and you do 1M lookups instead of 12.

`basic_common_view::unchecked_refresh()` scans the candidate pools and records `index`, the smallest one. `handle()` returns it. `refresh()` re-runs the scan when pools change.

Then in `view_iterator::valid()`:

```cpp
(Get == 1u) || (all_of(pools.begin(), pools.begin() + index, entt)
             && all_of(pools.begin() + index + 1, pools.end(), entt))
&& ((Exclude == 0u) || none_of(filter.begin(), filter.end(), entt))
```

Note the two ranges skipping `index` — you don't re-check the pool you're iterating. And note `Get == 1u` / `Exclude == 0u` are compile-time constants, so single-component views compile the entire filter chain away to nothing.

### 7.3 Iterator composition

`extended_view_iterator` wraps the base iterator and, on dereference, produces `tuple_cat(make_tuple(*it), pools->get_as_tuple(*it)...)` — the entity plus every requested component, assembled via `index_sequence` expansion inside an immediately-invoked generic lambda.

This is what makes `for (auto [e, pos, vel] : view.each())` work. The empty-type case from Module 3 slots in here: `get_as_tuple` returns an empty tuple for tag components, and `tuple_cat` just... skips it. No special case anywhere.

### 7.4 The `placeholder` trick

Look at how `filter` entries are initialized to a `placeholder` pointer rather than `nullptr`, and `filter_at()` translates back. A missing exclusion pool must behave as "excludes nothing" — but you also need to distinguish "not yet bound" from "bound to nothing." A dedicated non-null sentinel does that without an extra bool per slot.

### Exercises

1. **(2 hr)** Extend your Module 6 sparse set into a typed storage with paged payload. Prove pointer stability: hold a `T*`, insert 10,000 more elements, dereference it.
2. **(2 hr)** Write a two-component view over two of your storages. Implement smallest-pool selection. Benchmark against the naive "always iterate pool A" version with pool sizes of 1M and 100.
3. **(1.5 hr)** Build the `each()`-style extended iterator with `tuple_cat`. Get structured bindings working. Then verify at `-O2` on Compiler Explorer that the tuple machinery is entirely eliminated.
4. **(1 hr)** Add an empty tag type and confirm it allocates no payload and still composes in `each()`.

**Checkpoint:** you can explain why paged payload and paged sparse arrays exist for *different* reasons, and name the trade-off each accepts.

---

## Module 8 — Type erasure, three ways

**Read:** `src/entt/core/any.hpp`, then `src/entt/signal/delegate.hpp`, then `src/entt/poly/poly.hpp`.

Three different answers to "call code without knowing its type at compile time," each with a different cost profile. Studying them side by side is the fastest way to develop real judgment about type erasure.

### 8.1 `basic_any` — SBO plus a single-function vtable

Two template parameters, `Len` and `Align`, control the small-buffer size. `internal::in_situ<Type, Len, Align>` decides at compile time whether an object fits inline or must be heap-allocated. `Len == 0` specializes to a storage type with no buffer at all — a pointer-only `any`, which is exactly what you want when you know everything is heap-allocated anyway.

The interesting part is the dispatch. Instead of a table of N function pointers, there's **one**:

```cpp
template<cvref_unqualified Type>
static const void *basic_vtable(const request req, const basic_any &value, const void *other);
```

`request` is a private alias for `internal::any_request`, an enum of operations (copy, move, destroy, compare, get type info, …). One function pointer per `basic_any`, one `switch` inside it, and the compiler gets to see all the operations for a type together — better inlining opportunities and a smaller object than N pointers. Small style note while you're in there: the switch body opens with `using enum internal::any_request;` — C++20's `using enum` in the wild, keeping case labels unqualified without polluting any wider scope.

Then `any_policy` (`empty`, `ref`, `cref`, `embedded`, `dynamic`) tracks *ownership mode* separately, so the same type can be held by value, by reference, or by const reference without three different wrapper types. Read how `mode` interacts with the destructor: an `embedded` trivially-destructible type skips destruction entirely.

**Compare this against your standard library's `std::any` implementation.** Open libstdc++'s `<any>` and read its `_Manager` type. Same idea, different trade-offs. Note which one gives you a smaller `sizeof`.

### 8.2 `delegate` — the fastest callable wrapper you can write

`delegate<R(Args...)>` stores an untyped instance pointer and a function pointer with a fixed signature. Binding is done through template parameters — `delegate.connect<&foo>()` — so the target is known at compile time and the trampoline is a direct call the compiler can often inline through.

No allocation, ever. Two pointers. Compare `sizeof(entt::delegate<void(int)>)` against `sizeof(std::function<void(int)>)` and look at the generated code for a call through each. The difference is not subtle.

**Where this matters for you:** interrupt-adjacent code, callback tables in device drivers, event dispatch on a target with no heap. `std::function` is a bad fit for all three; `delegate` is a good one.

Also read `sigh.hpp` (signal handler) and `dispatcher.hpp` to see how delegates compose into an observer system, and how listener removal is handled during emission — the classic reentrancy hazard.

### 8.3 `poly` — concept-based external polymorphism

`poly` lets you define an interface as a template, list the operations, and get a type-erased handle over *any* type that satisfies it — without that type inheriting from anything. Non-intrusive polymorphism.

This is the most template-heavy file in the library and the one with the highest compile-time cost. Read it for the technique, but the practical lesson is the comparison: `poly` buys you non-intrusiveness and pays in compile time and (sometimes) an extra indirection. Know when that trade is right.

### Exercises

1. **(1 hr)** Build a table: `sizeof`, allocation behaviour, call overhead (measured), and compile-time cost for `std::function`, `entt::delegate`, a raw function pointer, a virtual call, and `entt::poly`. Fill it in with numbers you measured, not numbers you expect.
2. **(2.5 hr)** Implement your own `small_any<Len, Align>` with the single-function-vtable dispatch. Support copy, move, destroy, and type query. Verify with `nm --size-sort` that only the operations you use get emitted.
3. **(1.5 hr)** Implement a two-pointer delegate with compile-time member-function binding. Confirm on Compiler Explorer that a call through it at `-O2` inlines to the target body when the delegate is a local constant.
4. **(1 hr)** Take a `std::function`-based callback in code you own and swap in your delegate. Measure binary size and call cost before/after.

**Checkpoint:** someone asks you "should I use `std::function` here?" and you can answer with three follow-up questions and a defensible recommendation.

---

# Phase C — Concurrency

> **Ground rules for Phase C.** Every exercise gets built with `-fsanitize=thread` before you believe any result. Every claim about memory ordering gets checked against the actual code, not against what you remember about acquire/release. If you find yourself writing "this is probably fine," stop and write a test instead.

## Module 9 — The Chase–Lev work-stealing deque

**Read:** `taskflow/core/wsq.hpp`. Both `UnboundedWSQ` and `BoundedWSQ`.

The header cites the paper directly (Lê, Pop, Cohen, Zappa Nardelli, *Correct and Efficient Work-Stealing for Weak Memory Models*, PPoPP'13). Read the paper alongside the code. It's 10 pages and it's the reason the memory orders are what they are.

### 9.1 The contract

One **owner** thread does `push` and `pop` at the bottom. Many **thief** threads do `steal` from the top. Owner operates LIFO (hot, cache-warm tasks); thieves take FIFO (cold, coarse-grained tasks). That asymmetry is the whole design.

### 9.2 `pop` — read it line by line

```cpp
int64_t b = _bottom.load(memory_order_relaxed) - 1;
Array* a = _array.load(memory_order_relaxed);
_bottom.store(b, memory_order_relaxed);
atomic_thread_fence(memory_order_seq_cst);
int64_t t = _top.load(memory_order_relaxed);

if (t <= b) {
    item = a->pop(b);
    if (t == b) {                                   // last element — race with a thief
        if (!_top.compare_exchange_strong(t, t+1, seq_cst, relaxed)) {
            item = empty_value();                   // thief won
        }
        _bottom.store(b + 1, relaxed);
    }
} else {
    _bottom.store(b + 1, relaxed);                  // empty; restore
}
```

Questions you must be able to answer before moving on:

1. Why is the owner's `_bottom` load `relaxed` when a thief's is `acquire`?
2. What exactly does the `seq_cst` fence prevent? Construct the interleaving that breaks if you remove it. (Hint: the fence stops the `_bottom` store from being reordered after the `_top` load. Without it, owner and thief can both conclude they got the last element.)
3. Why `compare_exchange_strong` and not `weak` here?
4. Why does the owner CAS `_top` at all, when it's the *bottom* it operates on?

### 9.3 `steal`

```cpp
int64_t t = _top.load(memory_order_acquire);
atomic_thread_fence(memory_order_seq_cst);
int64_t b = _bottom.load(memory_order_acquire);
if (t < b) {
    Array* a = _array.load(memory_order_consume);
    item = a->pop(t);
    if (!_top.compare_exchange_strong(t, t+1, seq_cst, relaxed)) return empty_value();
}
```

Note the order: read `top`, fence, read `bottom`, *then* read the array pointer. Note also that the item is read **before** the CAS succeeds — which means a thief may read a slot that's concurrently being overwritten. This is benign only because the CAS then fails and the value is discarded. Convince yourself of that; it's the subtlest part of the algorithm and a common place to get it wrong when reimplementing.

### 9.4 ABA and the garbage list

`_top` is monotonically increasing and 64-bit. It never wraps in practice, so there's no ABA on the counter. But `resize()` allocates a new `Array` and the old one *cannot be freed* — a thief may still be holding a pointer to it. Look at `std::vector<Array*> _garbage`: old arrays are retained until the queue is destroyed.

That's a deliberate memory-vs-complexity trade (the alternative is hazard pointers or epoch-based reclamation). Understand why the simple choice is acceptable here: resizes are rare and bounded, so the garbage is bounded.

### 9.5 Cache-line discipline

```cpp
alignas(TF_CACHELINE_SIZE) std::atomic<int64_t> _top;
alignas(TF_CACHELINE_SIZE) std::atomic<int64_t> _bottom;
alignas(TF_CACHELINE_SIZE) std::atomic<Array*> _array;
int64_t _cached_top {0};   // owner-private, never read by thieves
```

Three separate cache lines because owner and thieves hammer different variables. `_cached_top` is a monotonic-lower-bound optimization: since `_top` only increases, a stale cached value is always a *conservative* estimate of occupancy, so the owner can often skip loading the real `_top` entirely. Read the comment in the source — it's a good example of documenting a correctness argument, not just an intent.

### Exercises

1. **(1 hr)** Answer all four questions in §9.2 in writing. Not in your head.
2. **(4 hr)** Implement the bounded (fixed-capacity, power-of-two mask) version from scratch. Do not look at the source while writing; look after.
3. **(2 hr)** Test it: one owner pushing/popping, N thieves stealing, with a global counter verifying every pushed item is consumed exactly once. Run under TSan. Then deliberately weaken one memory order (e.g. the `seq_cst` fence in `pop` → `acq_rel`) and see whether your test catches it. It probably won't on x86 — which is the lesson. Test on ARM if you have a board handy.
4. **(2 hr)** Add the unbounded version with growth and the garbage list. Then instrument it: how many resizes actually happen in your workload?
5. **(1 hr)** Compare against a naive `std::deque` + `std::mutex` queue at 1, 2, 4, 8 threads. Plot it.

**Checkpoint:** you can defend every single `memory_order` argument in `pop` and `steal` from first principles, and you have a test that fails on a weak-memory machine if you get one wrong.

---

## Module 10 — Sleeping without lost wakeups

**Read:** `taskflow/core/nonblocking_notifier.hpp`, then `Executor::_wait_for_task` and `_explore_task` in `taskflow/core/executor.hpp`.

This is the hardest module and the most valuable. Every thread-pool implementer eventually meets this problem and most get it wrong at least once.

### 10.1 The problem

A worker finds all queues empty and wants to sleep. Between "I checked and found nothing" and "I'm asleep," another thread pushes work and signals. The sleeper misses the signal and sleeps forever with work pending. Classic lost wakeup.

Spinning forever burns power (a real cost on battery devices and a real cost in a datacenter). A mutex+condvar per push is correct but adds a lock to the hot path. You want: **no synchronization on push when nobody is sleeping, correct wakeup when somebody is.**

### 10.2 The two-phase commit protocol

```cpp
notifier.prepare_wait(wid);      // announce intent to sleep
// ... re-check the predicate ...
if (found_work) notifier.cancel_wait(wid);
else            notifier.commit_wait(wid);   // actually park
```

The rule: **`prepare_wait` must be followed by exactly one of `commit_wait` or `cancel_wait`.** The header says so explicitly, and violating it is UB.

Why it works: `prepare_wait` publishes "a thread is about to sleep" *before* the predicate re-check. Any notifier that arrives after the check but before the park sees the pre-waiter count and will deliver a wakeup. The window is closed by ordering, not by a lock.

### 10.3 The packed state word

```cpp
_waiters[wid].epoch = _state.fetch_add(PREWAITER_INC, memory_order_relaxed);
```

`_state` is a single `uint64_t` carrying several bitfields: a **stack pointer** (index of the waiter list head — an intrusive lock-free stack of parked waiters), a **pre-waiter count**, and an **epoch** counter. Read the layout constants (`STACK_MASK`, `PREWAITER_MASK`, `EPOCH_MASK`, `EPOCH_INC`) and draw the word on paper before reading any of the functions.

The epoch is the ABA defence: a CAS on the state word bumps the epoch, so a stale read can't succeed even if the stack pointer happens to match.

This design descends from Eigen's `EventCount` (Dmitry Vyukov). Reading Vyukov's original alongside it is worthwhile.

### 10.4 The executor's use of it

`_wait_for_task` is where theory meets a real scheduler:

```
explore_task:
  _explore_task(...)              // randomized stealing, bounded attempts
  if (got work) return true

  _notifier.prepare_wait(w._id)   // 2PC opens here

  // fast path: no live topologies → queues provably empty
  if (_num_topologies == 0) { commit_wait; goto explore_task; }

  for each buffer:  if (!empty) { cancel_wait; sticky_victim = ...; goto explore_task; }
  for each worker:  if (!empty) { cancel_wait; sticky_victim = ...; goto explore_task; }
  if (w._done)      { cancel_wait; return false; }

  _notifier.commit_wait(w._id)
  goto explore_task;
```

Things to notice:

- **Every exit path calls exactly one of cancel/commit.** Trace all six. That discipline is why the `goto` is here — it's clearer than nested breaks, and the source is honest about that.
- **The re-check after `prepare_wait` is the entire point.** Remove it and you have a lost-wakeup bug that shows up once a week in production.
- **`_num_topologies == 0` is a `relaxed` load used as a hint.** The comment explains why relaxed is safe: a missed update is caught by the 2PC guard anyway. This is the right way to reason about relaxed — not "it's faster" but "here is the argument that a stale value is harmless."
- **`sticky_victim`** — after a successful steal, the worker remembers who it stole from and tries them first next time. Producer/consumer relationships in a task graph are stable, so this converts random stealing into near-directed stealing. Cheap heuristic, large effect.
- **`_explore_task` bounds its attempts** (`MAX_STEALS`, then `yield()`, then a hard cap at `150 + MAX_STEALS`) rather than spinning forever. Read the exact constants and think about what workload they're tuned for.

> **Read the right one.** `executor.hpp` contains **two** definitions of `_explore_task`: the live one, and a near-duplicate immediately below it wrapped in `/* ... */`. They are nearly identical, which is exactly what makes this a trap — you can read the dead one, understand it perfectly, and be subtly wrong. Confirm you're in the live version before drawing conclusions. This is worth practicing generally: `grep -n` for a function name before studying it, and check whether you got more hits than you expected.

- **Victim sampling excludes self**, and the way it does so is worth stealing:

  ```cpp
  vtm = w._rdgen() % (MAX_VICTIM - 1);
  if (vtm >= w._id) vtm++;
  ```

  This maps a draw over `[0, MAX_VICTIM-1)` onto `[0, MAX_VICTIM) \ {w._id}` — a sample from a set minus one element, with one modulo and one predicated increment, no rejection loop. Rejection sampling ("draw again if you got yourself") is the obvious approach and has unbounded worst-case latency; this has none. The constructor's guarantee that `MAX_VICTIM >= 2` is what makes it total. (Pedantry that a course preaching precision owes you: `%` over a non-power-of-two range has modulo bias, so "uniform" is approximate — negligible here, but know that you're waving it away. Also note the commented-out copy lacks the self-exclusion and can pick itself as a victim — one of the few real differences between the two versions, and a good reason to have checked which one you were reading.)

### Exercises

1. **(1 hr)** Draw the `_state` word layout to scale. Label every field. Compute the maximum number of workers the encoding supports.
2. **(3 hr)** Build a *broken* thread pool: check-then-sleep with no 2PC. Write a stress test that reliably reproduces the lost wakeup (hint: many short tasks, many workers, a tight producer loop). Getting a hang to reproduce deterministically is itself a valuable skill.
3. **(4 hr)** Fix it with a two-phase notifier of your own. Start with mutex+condvar internals and a correct 2PC protocol — get the protocol right before you get it lock-free.
4. **(3 hr)** Replace the internals with a packed atomic state word and an intrusive waiter stack. TSan throughout.
5. **(1.5 hr)** Measure: idle CPU usage of (a) spin-only, (b) your 2PC pool, (c) `std::condition_variable` signalled on every push, under a bursty workload. The idle-power difference is the number that matters on a battery-powered target.
6. **(1 hr)** Add `sticky_victim` to your stealing loop and measure the change in steal-attempt count on a pipeline-shaped workload.

**Checkpoint:** you can explain the lost-wakeup window to a colleague using a two-column timeline diagram, and show precisely which instruction closes it.

---

## Module 11 — Scheduler and graph representation

**Read:** `taskflow/core/graph.hpp`, then `Executor::_invoke` and `_schedule` in `executor.hpp`, then `taskflow/core/topology.hpp`.

### 11.1 Nodes as variants

```cpp
using handle_t = std::variant<Static, Runtime, Subflow, Condition,
                              MultiCondition, Module, Async, DependentAsync, ...>;
```

Heterogeneous node kinds in one type without inheritance or heap indirection per node. Cost: `sizeof(Node)` is the max over all alternatives plus a discriminator.

Now look at how `_invoke` dispatches: `switch(node->_handle.index())` with `std::get_if` inside each case — **not** `std::visit`. Be precise about the evidence here, because it's a good habit: the source comment on that switch reads "switch is faster than nested if-else due to jump table," which justifies switch over *if-else chains*. It does not mention `std::visit`. The visit comparison is inference, not source-attested — so treat it as a question to answer with a compiler rather than a fact to memorize. Build both versions in a scratch TU and diff the codegen at `-O2`. The plausible advantages of index-switching are that each case can call a dedicated `_invoke_*_task` function (keeping per-kind logic in separate, individually optimizable functions rather than one visitor body) and that the jump table is explicit rather than emergent. Whether the optimizer actually produces different code is exactly the sort of thing you should stop guessing about.

While you're in `_invoke`, study the **continuation cache**: the `Node* cache` variable plus the `TF_INVOKE_CONTINUATION()` macro that does `node = cache; goto begin_invoke;`. When a finishing task has exactly one ready successor, the worker executes it *directly* — no push to the queue, no pop, no chance of another worker stealing a task that's already hot in this core's cache. Queue traffic only happens at fan-out. This is continuation-passing inside a work-stealing scheduler, and it's a large fraction of why linear chains of small tasks stay fast.

### 11.2 The successor/predecessor packing

```cpp
size_t _num_successors {0};
SmallVector<Node*, 4> _edges;
```

**One** vector holds both successors and predecessors, partitioned at `_num_successors`. Adding a successor:

```cpp
_edges.push_back(v);
std::swap(_edges[_num_successors++], _edges[_edges.size() - 1]);
v->_edges.push_back(this);
```

Push to the back, swap into the partition boundary, bump the boundary. O(1), one allocation, one cache line for typical fan-out. Read `_remove_successors` to see the inverse — it uses `std::remove` + `std::move` to re-compact both halves.

`SmallVector<Node*, 4>` is LLVM's small vector (vendored in `utility/small_vector.hpp`): 4 inline pointers, heap only beyond that. Most task nodes have ≤4 edges, so most graphs allocate nothing for topology.

### 11.3 Join counters

Each node has `std::atomic<size_t> _join_counter`. On completion, a task decrements each successor's counter; whoever drives it to zero schedules that successor. That's the entire dependency mechanism — no locks, no central scheduler state.

Read `_set_up_join_counter()` and think about the reset problem: a graph that runs twice must have its counters restored. When and by whom?

### 11.4 Bit-packed state

```cpp
nstate_t _nstate {NSTATE::NONE};              // non-atomic: single-threaded fields
std::atomic<estate_t> _estate {ESTATE::NONE}; // atomic: cross-thread fields
```

Two state words, split by *who touches them*. And note this comment in the `Async` node:

> use_count is packed into the lower 24 bits of `NodeBase::_estate` (`ESTATE::REFCOUNT_MASK`) to avoid a separate atomic and a `std::get_if` call on every AsyncTask copy/move/destroy.

A refcount stuffed into spare bits of an existing atomic. One RMW instead of two, no extra cache line. Priority is likewise packed into `_nstate` with a mask and shift. This is exactly the kind of bit-packing you'd do in a register map, applied to a scheduler.

### 11.5 Exceptions across worker threads

Look at the `try`/`catch(...)` wrapping the work-stealing loop, `std::exception_ptr ptr`, and how it flows to `scheduler_epilogue`. Then find where an exception in a task marks `ESTATE::EXCEPTION` and cancels the topology.

Propagating an exception from a worker thread to the thread that called `wait()` is a genuinely hard design problem — think about what should happen when *three* tasks throw simultaneously. Then read what Taskflow does. Also note `TF_DISABLE_EXCEPTION_HANDLING`, which compiles the whole mechanism out — a build knob you'd want on a target compiled with `-fno-exceptions`.

### Exercises

1. **(2 hr)** Compute `sizeof(tf::Node)` on your platform. Then work out, from the variant alternatives, *which* alternative is driving the size. Propose a change that would shrink it and estimate what it would cost elsewhere.
2. **(3 hr)** Implement the partitioned edge vector yourself, with add/remove for both successors and predecessors. Property-test that the partition invariant holds after randomized operations.
3. **(3 hr)** Build a minimal DAG executor: nodes with join counters, a thread pool with your Module 9 queue and Module 10 notifier, and topological execution. This is the integration point for all of Phase C.
4. **(1.5 hr)** Add exception propagation: a throwing task should cancel the graph and rethrow from `wait()`. Decide and document what happens with multiple simultaneous exceptions.
5. **(1 hr)** Pack a small refcount into spare bits of an existing atomic in your own code. Measure the RMW count before and after.

**Checkpoint:** you can sketch the complete lifecycle of one task node from graph construction to completion, naming every atomic operation involved.

---

## Module 12 — Parallel algorithm design

**Read:** `taskflow/algorithm/partitioner.hpp`, `for_each.hpp`, `reduce.hpp`, then `pipeline.hpp` and `taskflow/core/runtime.hpp`.

### 12.1 Partitioners as policy objects

First, get the structure right, because it's easy to misread: there are **four partitioner classes** (`StaticPartitioner`, `DynamicPartitioner`, `GuidedPartitioner`, `RandomPartitioner`), each deriving from `PartitionerBase<C>` and providing `loop()` / `loop_until()` — but the `PartitionerType` enum has only **two** values, `STATIC` and `DYNAMIC`. Guided and Random both report `type() == DYNAMIC`.

That asymmetry is deliberate and it's the design lesson of the file. The enum encodes the *scheduling contract* — the only thing the algorithm skeleton (`for_each`, `reduce`, …) needs to branch on: STATIC means "ranges are pre-assigned per worker, no shared state," DYNAMIC means "workers pull chunks from shared coordination." The *chunk-sizing strategy* (uniform, guided shrink, random) lives entirely inside each partitioner's `loop()` and never leaks into the algorithm. Contract in the enum, strategy in the policy type — two customization axes, deliberately kept at different visibility levels.

`StaticPartitioner::adjusted_chunk_size`:

```cpp
return _chunk_size ? _chunk_size : N/W + (w < N%W);
```

Zero means "auto": divide evenly, distribute the remainder one item at a time to the first `N%W` workers. Then `loop()` strides by `W * chunk_size`, giving each worker an interleaved set of chunks.

Compare with `GuidedPartitioner`: large chunks first (amortize scheduling overhead) shrinking toward the end (fine-grained load balancing on the tail). Read how the shrink factor is chosen.

**The design lesson:** the partitioner is a *template parameter with a default*, so the common case costs nothing and the specialist case requires no library change. The `closure_wrapper` hook lets a user inject per-chunk setup/teardown (thread-local buffers, NUMA pinning) without the algorithm knowing about it.

For your work: on a heterogeneous or asymmetric core layout — big.LITTLE, or a core with more interrupt load than the others — static partitioning is exactly wrong and guided is exactly right. Being able to articulate why is the payoff here.

### 12.2 `corun` and the recursion problem

If a task calls `taskflow.run(...).wait()` from inside a worker, that worker blocks and you've lost a thread — do it enough and you deadlock. `Runtime::corun()` instead makes the blocked worker *participate in executing* the nested graph until it completes.

Read `runtime.hpp` for how the worker re-enters the scheduling loop with a completion predicate. This is the same problem TBB solves with `task_arena` and Grand Central Dispatch famously does not solve well.

### 12.3 Pipeline scheduling

`pipeline.hpp` implements a scheduling framework where tasks form stages and tokens flow through them, with per-stage `SERIAL`/`PARALLEL` types. Read how a serial stage's ordering is enforced without a lock (look for the per-line atomic join counters).

`data_pipeline.hpp` is the typed variant where each stage's output type feeds the next — a nice study in variadic template plumbing over a runtime scheduling structure.

### Exercises

1. **(2.5 hr)** Implement static, dynamic (atomic cursor), and guided partitioners behind a common interface. Benchmark all three on: (a) uniform work per item, (b) work proportional to index, (c) random work with a heavy tail. Each partitioner should win one of these.
2. **(1.5 hr)** Reproduce the nested-blocking deadlock, then fix it with a `corun`-style helper.
3. **(3 hr)** Build a three-stage pipeline (serial → parallel → serial) on your Module 11 executor. Verify output ordering out of the final serial stage.
4. **(1 hr)** Use `closure_wrapper` (or your equivalent) to give each worker a thread-local scratch buffer in a parallel reduce. Measure the allocation reduction.

**Checkpoint:** given a workload description, you can pick a partitioner and predict, roughly, the load imbalance you'll get from the wrong one.

---

## ★ Capstone

Pick one. Budget 20–40 hours. The requirement is that it uses techniques from **both** halves of the course.

**Option A — Embedded device orchestrator.**
An ECS-style component store (Modules 5–7) holding peripheral state, driven by a small DAG scheduler (Modules 9–11) with a 2–4 worker pool. Constraints: no RTTI, no exceptions, no dynamic allocation after init. Use the Module 4 shim to swap in fixed-capacity containers. Target something real — a Cortex-A running embedded Linux, or an ESP32-S3 with FreeRTOS underneath your pool. Deliverable: a build that runs on the target, plus measured worst-case latency for a full graph pass.

**Option B — Reactive dataflow engine.**
Values as entities, dependencies as a DAG, recomputation as a partial topological execution. Add change propagation: touching a value dirties only its transitive successors. Uses the sparse set for dependency tracking and the executor for parallel recomputation. Deliverable: correctness under concurrent mutation, plus a benchmark against naive full recomputation.

**Option C — Contribute upstream.**
Both repos have open issues. Read `CONTRIBUTING`, find something small, do it properly. Deliverable: a merged PR, or a well-argued one that wasn't merged and you understand why.

**Whatever you build, produce these artifacts:**

- A `docs/design.md` explaining every non-obvious choice, with the alternative you rejected.
- A benchmark suite with numbers, not adjectives.
- A TSan-clean build for anything concurrent.
- A `static_assert` battery documenting layout and trait assumptions.

---

## Appendix A — Techniques index

Quick reference for when you need to *find* a technique again rather than learn it.

| Technique | Where | Module |
|---|---|---|
| Type name from `__PRETTY_FUNCTION__` | `core/type_info.hpp` | 1 |
| SFINAE on constant-evaluability | `core/type_info.hpp` | 1 |
| `int`/`char` overload ranking | `core/type_info.hpp` | 1 |
| Compile-time FNV-1a + UDL | `core/hashed_string.hpp` | 1 |
| Cross-DSO static merging | `config/config.h` | 1 |
| Constrained partial specialization | `entity/component.hpp` | 2 |
| Policy inferred from type properties | `entity/component.hpp` | 2 |
| Customization-point ladder | `entity/component.hpp` | 2 |
| Type list algebra | `core/type_traits.hpp` | 2 |
| EBO with disambiguating tag | `core/compressed_pair.hpp` | 3 |
| Zero-cost empty components | `entity/component.hpp`, `entity/storage.hpp` | 3 |
| `__has_include` extension seam | `stl/*.hpp` | 4 |
| Generational index handles | `entity/entity.hpp` | 5 |
| Trait inheritance via `requires requires` | `entity/entity.hpp` | 5 |
| Paged sparse array | `entity/sparse_set.hpp` | 6 |
| `fast_mod` with power-of-two assert | `core/bit.hpp` | 6 |
| Intrusive free list in tombstoned slots | `entity/sparse_set.hpp` | 6 |
| Partitioned array (`swap_only`) | `entity/sparse_set.hpp` | 6 |
| Private-virtual (NVI) seam | `entity/sparse_set.hpp` | 6 |
| Paged payload for pointer stability | `entity/storage.hpp` | 7 |
| Backward iteration for safe mid-loop removal | `entity/storage.hpp` | 7 |
| Entity recycling via `swap_only` partition | `entity/storage.hpp` | 6 |
| Smallest-pool selection | `entity/view.hpp` | 7 |
| Iterator composition via `tuple_cat` | `entity/view.hpp` | 7 |
| Non-null sentinel (`placeholder`) | `entity/view.hpp` | 7 |
| SBO with compile-time fit check | `core/any.hpp` | 8 |
| Single-function-pointer vtable | `core/any.hpp` | 8 |
| Ownership mode as a policy enum | `core/any.hpp` | 8 |
| Two-pointer compile-time-bound delegate | `signal/delegate.hpp` | 8 |
| Non-intrusive external polymorphism | `poly/poly.hpp` | 8 |
| Chase–Lev deque | `core/wsq.hpp` | 9 |
| Cache-line isolation of contended atomics | `core/wsq.hpp` | 9 |
| Monotonic-bound caching (`_cached_top`) | `core/wsq.hpp` | 9 |
| Retained-garbage reclamation | `core/wsq.hpp` | 9 |
| Two-phase commit wait | `core/nonblocking_notifier.hpp` | 10 |
| Packed atomic state + epoch anti-ABA | `core/nonblocking_notifier.hpp` | 10 |
| Relaxed loads justified by a downstream guard | `core/executor.hpp` | 10 |
| Sticky victim stealing | `core/executor.hpp` | 10 |
| Uniform sampling from a set minus one element | `core/executor.hpp` | 10 |
| Bounded steal attempts with yield | `core/executor.hpp` | 10 |
| Variant nodes | `core/graph.hpp` | 11 |
| Partitioned successor/predecessor vector | `core/graph.hpp` | 11 |
| Small vector with inline capacity | `utility/small_vector.hpp` | 11 |
| Atomic join counters | `core/graph.hpp` | 11 |
| Refcount packed into spare state bits | `core/graph.hpp` | 11 |
| State split by ownership (atomic vs not) | `core/graph.hpp` | 11 |
| Direct continuation via invoke cache | `core/executor.hpp` | 11 |
| Cross-thread exception propagation | `core/executor.hpp` | 11 |
| Partitioner as defaulted template policy | `algorithm/partitioner.hpp` | 12 |
| Contract enum vs strategy class split | `algorithm/partitioner.hpp` | 12 |
| Closure wrapper injection point | `algorithm/partitioner.hpp` | 12 |
| Cooperative blocking (`corun`) | `core/runtime.hpp` | 12 |
| Lock-free serial pipeline stages | `algorithm/pipeline.hpp` | 12 |

## Appendix B — Companion reading

- Lê, Pop, Cohen, Zappa Nardelli — *Correct and Efficient Work-Stealing for Weak Memory Models* (PPoPP'13). Required for Module 9; the header cites it directly.
- Blumofe & Leiserson — *Scheduling Multithreaded Computations by Work Stealing*. The original theory behind Module 11.
- Dmitry Vyukov's `EventCount` (in Eigen's `ThreadPool`). The ancestor of Module 10's notifier.
- Herb Sutter — *atomic<> Weapons* (talks). The best available grounding for Phase C.
- Chandler Carruth — *Efficiency with Algorithms, Performance with Data Structures* (CppCon 2014). The conceptual case for Modules 6–7.
- Sean Parent — *Inheritance Is the Base Class of Evil*. Read before Module 8.
- Skypjack's blog (the EnTT author) — long-form posts on the ECS design decisions in Phase B.
- cppreference on `std::memory_order`. Keep it open all of Phase C.

## Appendix C — Self-assessment

You're done when you can do all of these without reference:

- [ ] Explain why `type_index` needs visibility control across shared objects.
- [ ] Write the three-level customization ladder from memory.
- [ ] State the exact code that breaks a tag-less `compressed_pair`.
- [ ] List the standard-library surface a nontrivial library actually needs.
- [ ] Choose a bit split for a generational handle given live-count and churn requirements.
- [ ] Draw a sparse set with holes, including tombstone contents.
- [ ] Explain paged sparse vs paged payload — different motivations, different trade-offs.
- [ ] Recommend between `std::function`, a delegate, a virtual call, and a raw pointer, with reasons.
- [ ] Defend every memory order in Chase–Lev `pop` and `steal`.
- [ ] Diagram the lost-wakeup window and the instruction that closes it.
- [ ] Sketch a task node's full lifecycle with every atomic operation named.
- [ ] Predict load imbalance from the wrong partitioner for a given workload shape.
