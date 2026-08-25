# Module 5 — Bit-packed handles

Source under study: `entt/entity/entity.hpp` (`internal::entt_traits`,
`basic_entt_traits`, `null_t`, `tombstone_t`).
Reimplementation: `src/acpp/handle.hpp`.

| Exercise | Deliverable |
|---|---|
| 1 — `handle_allocator<T, IndexBits, VersionBits>` | `handle_allocator.cpp`, `compile_fail_oversized_split` |
| 2 — version wraparound, decided and documented | `version_wraparound.cpp`, below |
| 3 — randomized property test | `handle_property_test.cpp` |
| checkpoint | below |

---

## Checkpoint: 64-bit budget, ≤10M live objects, staleness detectable for a year at 1000 releases/sec

**Answer: 24 bits of index, 40 of version.** Working:

*Index.* 10M live objects needs ⌈log₂(10 × 10⁶)⌉ = **24 bits** (16,777,216).
23 gives 8.4M, which is short. One index value is reserved for null, so the
usable count is 2²⁴ − 1 = 16,777,215 — still comfortably above 10M.

*Version.* A year is ≈3.15 × 10⁷ seconds, so 1000 releases/sec is
**3.15 × 10¹⁰ releases** in total.

The tempting move is to divide by the slot count: 3.15 × 10¹⁰ / 10⁷ ≈ 3150
reuses per slot, which fits in 12 bits. **That is the wrong calculation**, and
getting it wrong is the whole point of the question. Releases are not
distributed evenly. A workload with one hot slot — a connection that reconnects,
a buffer that cycles, a session that renews — puts a large fraction of the churn
through a single index. The requirement says *detect staleness for a year*, and a
requirement about detection is a worst-case requirement.

So size the version for the pathological case: one slot absorbing all
3.15 × 10¹⁰ releases needs ⌈log₂(3.15 × 10¹⁰)⌉ = **35 bits**.

24 + 35 = 59, leaving 5 bits spare. Spend them on the version rather than
banking them: **24 + 40 = 64**, giving 1.1 × 10¹² reuses of a single slot, or
about **35 years** of the entire system's churn hitting one index. Spare bits in
a handle have no other use, and the version is the field whose exhaustion is
silent.

*What that costs:* nothing at runtime — it is the same 64-bit word either way.

*What would change the answer:* if the handles were serialised into a format
with a fixed 32-bit field, the budget collapses and the honest answer becomes
"20/12 plus an out-of-band epoch", not "squeeze it". If detection needs to be
*exact* rather than *for a year*, the answer is `exhaustion_policy::retire`
below, at the cost of leaking an index per exhausted slot.

## The layout

`basic_handle_traits` derives the shift with `popcount(index_mask)` rather than
carrying a separate constant, so changing the mask is a one-line edit. The
`static_assert` that the mask is `2ⁿ − 1` lives inside `to_index`, at the point
of use.

**`null` and `tombstone` have the same bit pattern and different comparisons**,
and that asymmetry is the design:

| | bits | `operator==` compares |
|---|---|---|
| `null` | index=all ones, version=all ones | the **index** part only |
| `tombstone` | index=all ones, version=all ones | the **version** part only |

`null` asks "is this any slot at all?"; `tombstone` asks "is this slot a hole?".
Module 6 needs the second: a tombstoned slot in the packed array stores *another
slot's index* in its index bits while still comparing equal to `tombstone`. That
is what makes the intrusive free list possible, and a sentinel compared as a
whole word could not express it. `handle_layout.cpp` pins exactly that encoding.

`next()` steps over the reserved all-ones version branchlessly:

```cpp
const auto version = to_version(value) + 1u;
return construct(to_integral(value), version + (version == version_mask));
```

A live handle holding `version_mask` would compare equal to `tombstone`, so the
reserved value costs exactly one of the 2^VersionBits generations.

## Exercise 2 — the wraparound decision

Three policies are implemented so the trade is visible rather than argued:

| policy | leaks | detection after exhaustion | for |
|---|---|---|---|
| `recycle` (default) | nothing | best-effort — an old handle can validate again | general use, with enough version bits |
| `retire` | one index per exhausted slot | exact, forever | security boundaries, long-lived servers |
| `trap` | — | stops the program | when reaching this point means a design error upstream |

**This project defaults to `recycle`**, matching EnTT, for one reason: the
alternative leaks, and a slow leak in a long-running system is a worse failure
than a detection gap that correct sizing prevents. `recycle` puts the burden on
the bit split, which is a decision made once at design time with the arithmetic
above; `retire` puts it on the workload, which nobody controls.

Measured on a 4/4 split (15 slots, 15 usable versions), from
`version_wraparound.cpp`:

- **recycle** — after a full lap the version returns to its original value and a
  handle from the previous lap validates again. The test asserts that
  *deliberately*, because pretending otherwise would be the dishonest version of
  this module.
- **retire** — the slot is bumped to the reserved version, never returned to the
  free list, `retirements()` increments, and no handle to it ever validates
  again.

## The bug the exercises caught

`allocate()`'s exhaustion guard was `versions.size() > max_slots`, which lets the
allocator issue index == `index_mask`. That index **is** the null handle. The
allocator would have returned a perfectly valid-looking handle that every
`is_null` check treats as "allocation failed" — the kind of bug that surfaces
once, in production, under memory pressure.

The usable range is `[0, index_mask)`: `max_slots` slots, not `max_slots + 1`.
Found by the one check in `handle_allocator.cpp` that drives a tiny 4/4 split to
exhaustion, which is the argument for testing the boundary with a split small
enough to reach rather than only the realistic one.

A second, quieter consequence of custom splits: the free `acpp::to_index` /
`to_version` / `acpp::null` read the *type's default* split, not the allocator's.
Ask a handle from a 4/4 split about `acpp::null` and you compare against a
20-bit mask. `handle_allocator` therefore exposes `index_of`, `version_of`,
`null_handle` and `is_null` of its own, and the tests use those.

## Exercise 3 — the property test

One property: *no stale handle ever validates.* 200 seeds × 400 randomized
allocate/release operations, checking after **every** step against **every**
handle ever released — not just the most recent, because the interesting failure
is an old handle becoming valid again several reuses later.

The converse is checked too: *every live handle still validates*. Without it, an
`is_valid` that always returned false would pass.

Plus a directed case the random walk is unlikely to produce: 500 generations of
the same slot, all 500 handles retained, none valid, one slot used.

## Techniques logged

Added to `docs/notes.md`: generational index handles, trait inheritance via
`requires requires`, sentinels that compare on one field, branchless
reserved-value skipping.
