# Module 7 — Storage and view iteration

Source under study: `entt/entity/storage.hpp`, `entt/entity/view.hpp`.
Reimplementation: `src/acpp/storage.hpp`, `src/acpp/view.hpp`.

| Exercise | Deliverable |
|---|---|
| 1 — typed storage with paged payload, pointer stability proved | `paged_payload.cpp` |
| 2 — two-component view, smallest-pool selection, benchmarked | `view_composition.cpp`, `view_bench.cpp` |
| 3 — `each()` via `tuple_cat`, structured bindings, codegen verified | `view_composition.cpp`, `view_codegen.cpp` |
| 4 — an empty tag: no payload, still composes | `view_composition.cpp` |

---

## Checkpoint: paged sparse and paged payload exist for different reasons

Same mechanism, opposite motivations. Getting this straight is the module.

| | indexed by | the problem | what paging buys | what it costs |
|---|---|---|---|---|
| **sparse** | entity id | ids are *sparse*; a flat array sized to the largest id wastes memory in proportion to it | waste bounded to one page (4096) | one indirection per lookup |
| **payload** | packed position | positions are dense, so memory is *not* the problem | **pointer stability** — pages are never reallocated, so an element's address never changes | one indirection per access, and iteration must be page-aware |

A flat `std::vector<T>` payload would be perfectly memory-efficient and would
invalidate every pointer, iterator and reference on growth. That is the trade
paging exists to refuse.

**Pointer stability under growth is not pointer stability under erasure.**
`paged_payload.cpp` checks both halves, because conflating them is the easy
mistake: 100 held addresses survive 10,000 further inserts, *and*
`swap_and_pop` erasure genuinely does move the last element's address. A type
that needs stable addresses across erasure has to be on `in_place` — which,
for a non-movable type, `component_traits` infers for it.

### Where Module 2's inference finally cashes in

`basic_storage::storage_policy` is derived, not configured:

```cpp
static constexpr deletion_policy storage_policy =
    traits_type::in_place_delete ? deletion_policy::in_place : deletion_policy::swap_and_pop;
```

And the relocation hooks are guarded:

```cpp
static constexpr bool relocatable =
    stl::is_move_constructible_v<Type> && stl::is_move_assignable_v<Type>;
```

Without the guard, a non-movable component **fails to compile** inside
`swap_or_move` — on a code path it can never take, because the policy that would
call it was never selected for it. The `if constexpr` is what turns Module 2's
"this is a correctness decision, not a preference" from a claim into something
the compiler enforces. The `else` branch traps rather than being
`std::unreachable()`: if the argument is ever wrong, a trap is a stack trace and
UB is a silent corruption.

## Backward iteration, and the two guarantees that fall out of it

`begin()` sits at the **high** end of the packed array and `++` walks toward 0.
`index()` returns `offset - 1`, and `operator-` and `operator<=>` are both
flipped to match, so `it1 < it2` still means "it1 comes first".

Two guarantees, and they are the same decision seen from two directions:

- **Erasing the current element is safe.** Under swap-and-pop the replacement
  comes from the highest live index — a slot this iterator has *already passed* —
  so nothing is skipped. Proved in `sparse_set_invariants.cpp`: 50 elements,
  erase every third mid-iteration, exactly 50 visits and no repeats.
- **Elements created mid-iteration are not visited.** New elements are appended
  at high packed indices, which are *behind* `begin()`.

Neither is a happy accident, and neither is documented by the type system, so
both are tests.

## Smallest-pool selection, measured

`refresh()` scans the candidate pools and records the index of the smallest;
`handle()` returns it. Measured on 1,000,000 positions and 100 velocities
spread across the whole id range, gcc `-O2`:

| | time |
|---|---|
| smallest-pool first | **0.036 ms** |
| always lead with `positions` | 9.630 ms |
| ratio | **266×** |

The ratio is roughly the pool-size ratio (10,000×) discounted by the fact that
the naive version's per-entity work is a cheap `contains()` on a warm sparse
array, while the smart version pays a cold random lookup per hit.

A view holds **pointers, not a snapshot**, so "smallest" can go stale.
`view_composition.cpp` checks both sides of that: after growing the velocity pool
past the position pool, the view still leads with velocities until `refresh()` is
called, and leads with positions afterwards. Silent staleness would be the worse
bug, so the test pins the current behaviour rather than pretending it re-scans.

## The filter chain, and what compiles away

```cpp
(!pools[index]->is_tombstone(entt))
&& ((Get == 1u) || (all_of(pools.begin(), pools.begin() + index, entt)
                    && all_of(pools.begin() + index + 1, pools.end(), entt)))
&& ((Exclude == 0u) || none_of(filter.begin(), filter.end(), entt))
```

`Get` and `Exclude` are compile-time constants, so a single-component view with
no exclusions compiles the entire chain away. The two ranges skipping `index`
are the other detail: there is no point re-checking the pool you are iterating.

**A trap this file hit:** `all_of` and `none_of` are called on `std::array`
iterators, so an *unqualified* call also finds `std::all_of` through ADL and is
ambiguous. Every call site is `internal::`-qualified, which disables ADL. A real
hazard for any library that re-exports standard names — as Module 4's shim does.

## Exercise 3 — the codegen, and a harness bug it exposed

`-O2`, gcc 13.3, checked by `codegen_view_*`:

- `acpp_probe_single_pool_sum` — a plain loop: `shrq $10` / `andl $1023` for the
  page split, `addss` for the accumulate. **No call, no tuple in memory.** The
  immediately-invoked generic lambda, the `index_sequence` expansion, the
  `tuple_cat` and the structured binding all evaporate.
- `acpp_probe_paged_access` — `shrq $10` and `andl $1023`, no `div`. The claim
  that "a division per element is fine because the page size is a compile-time
  power of two" is true here, and now checked on every build.
- `acpp_probe_two_pool_sum` — keeps the filter (it must), materialises no tuple.

### Two bugs, found in the right order

**The first was in `storage::each()`.** It was built on the same pool-lookup
iterator the *view* uses, which does `get_as_tuple(*it)` — a sparse-array lookup
per element. For a view that is unavoidable: it leads with one pool and must look
the entity up in the others. For a storage iterating *itself* it is pure waste,
because the payload is at the same packed position. Fixed by adding
`extended_storage_iterator`, which zips the two iterators and advances both.

**The second was in the harness, and it is the one worth remembering.**
`acpp_asm_probe` depended only on its `.cpp`, so the `.s` file was **not
regenerated when a header changed** — the codegen tests kept passing against
assembly nobody had produced since the header edit. A stale codegen check is
worse than no codegen check. Fixed with `-MD -MF` plus `DEPFILE` on the custom
command.

The fix is now pinned: `codegen_view_single_pool` forbids `$4095`, the
`ACPP_SPARSE_PAGE - 1` mask, which appears if and only if `each()` went back to
looking entities up. `ACPP_PACKED_PAGE - 1` is 1023, so the two are
distinguishable in the assembly.

## Exercise 4 — the tag component

```cpp
static_assert(stationary_storage::page_size == 0u);
static_assert(std::is_same_v<decltype(tags.get_as_tuple(e)), std::tuple<>>);
static_assert(std::tuple_size_v<decltype(view.each())::iterator::value_type> == 2u);
```

`page_size == 0` selects a `basic_storage` specialization with **no payload
member at all**. `get_as_tuple` returns `std::tuple<>`, `tuple_cat` drops it, and
`for(auto [entity, pos] : view.each())` has two bindings instead of three. There
is no special case anywhere downstream — the abstraction survives the
optimisation, which is the part that is actually hard.

## Techniques logged

Added to `docs/notes.md`: paged payload for pointer stability, backward
iteration, smallest-pool selection, iterator composition via `tuple_cat`, the
non-null `placeholder` sentinel, and the ADL hazard.
