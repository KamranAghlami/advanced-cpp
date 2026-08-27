# Module 6 — The sparse set

Source under study: `entt/entity/sparse_set.hpp`, `entt/core/bit.hpp`.
Reimplementation: `src/acpp/sparse_set.hpp`, `src/acpp/bit.hpp`.

| Exercise | Deliverable |
|---|---|
| 1 — paged sparse array + `swap_and_pop`, invariant under randomized ops | `sparse_set_invariants.cpp` |
| 2 — `in_place` with the intrusive free list, LIFO reuse | `in_place_free_list.cpp` |
| 3 — iteration vs `std::unordered_map`, measured | `sparse_set_bench.cpp`, below |
| 4 — the private-virtual seam + a derived payload class | `virtual_seam.cpp` |
| §6.3 — `swap_only` | `swap_only_partition.cpp` |

---

## Checkpoint: the memory layout of a sparse set with three holes

Nine elements pushed (ids 0–8, all version 0), then ids 0, 3 and 6 erased under
`in_place`. `head == 6`.

```
packed  ┌────────┬───┬───┬────────┬───┬───┬────────┬───┬───┐
 index  │   0    │ 1 │ 2 │   3    │ 4 │ 5 │   6    │ 7 │ 8 │
        ├────────┼───┼───┼────────┼───┼───┼────────┼───┼───┤
 value  │ idx=MAX│ 1 │ 2 │ idx=0  │ 4 │ 5 │ idx=3  │ 7 │ 8 │
        │ ver=T  │   │   │ ver=T  │   │   │ ver=T  │   │   │
        └───▲────┴───┴───┴───▲────┴───┴───┴───▲────┴───┴───┘
            │  (end of list) │                │
            └────────────────┴────────────────┴──── head = 6

sparse  page 0 (4096 entries, allocated on first touch)
        ┌──────┬───┬───┬──────┬───┬───┬──────┬───┬───┬──────────┐
   e =  │  0   │ 1 │ 2 │  3   │ 4 │ 5 │  6   │ 7 │ 8 │  9..4095 │
        ├──────┼───┼───┼──────┼───┼───┼──────┼───┼───┼──────────┤
        │ null │ 1 │ 2 │ null │ 4 │ 5 │ null │ 7 │ 8 │   null   │
        └──────┴───┴───┴──────┴───┴───┴──────┴───┴───┴──────────┘
```

**Exactly what is in each tombstoned `packed` slot** — the part the checkpoint is
really asking about:

| slot | entity bits | version bits | means |
|---|---|---|---|
| `packed[6]` | `3` | tombstone (all ones) | hole; next hole is at position 3 |
| `packed[3]` | `0` | tombstone | hole; next hole is at position 0 |
| `packed[0]` | `index_mask` | tombstone | hole; end of the list |

The entity bits of a freed slot hold **the index of the previously freed slot**,
not an entity. `head` points at the most recent hole. So the free list costs
nothing: it lives inside the array it manages. The tombstone version is what
keeps a hole distinguishable from a live entry during iteration, and — because
Module 5's `tombstone_t::operator==` compares *only the version part* — a slot
can carry an arbitrary index and still read as a hole. That comparison
asymmetry, which looked like a curiosity in Module 5, is load-bearing here.

Verified directly rather than described: `in_place_free_list.cpp` walks the chain
by hand and asserts `[6, 3, 0]`.

## The three policies, and why each exists

| | array stays dense | positions stable | destroys | free list |
|---|---|---|---|---|
| `swap_and_pop` | yes | **no** | yes | none needed |
| `in_place` | no (holes) | **yes** | yes | intrusive, in the packed array |
| `swap_only` | yes | no | **no** | the partition itself |

`swap_and_pop` is the default and the cheapest: move the last element into the
hole, shrink. Its cost is that the moved element's position changed, so anything
holding a position — or a pointer into a parallel payload array — is now wrong.

`in_place` exists for exactly that case. Nothing moves, so a `T*` into the
payload stays valid for the element's whole lifetime. You pay in iteration:
`in_place_free_list.cpp` shows the iterator walking holes as well as elements,
and `compact()` is how you settle the debt once, at a point you choose.

`swap_only` is the one that explains entity recycling. Nothing is destroyed; the
array is partitioned at `head`, live below and released above. Releasing swaps
the element up across the boundary and bumps its version; **recycling is one swap
back down**. The registry's entity storage uses this policy, which is why
entity recycling needs no free-list structure at all — the partition *is* the
free list. `swap_only_partition.cpp` checks that recycling does not grow the
packed array, and that a *retired* generation is refused rather than silently
resurrected.

`policy_to_head()` initialises `head` differently per policy without a branch:

```cpp
return max_size * static_cast<size_type>(mode != deletion_policy::swap_only);
```

`swap_only` starts with an empty live partition (`head == 0`); the other two
start with an empty free list (`head == max_size`). One expression, two meanings,
because the two things `head` denotes are complementary.

## Paging

`sparse` is indexed by **entity index**, which can be arbitrarily spread out. A
flat array sized to the largest id wastes memory catastrophically — an id of one
million with two elements in the set would allocate a million slots. Paging
bounds the waste to one page (4096 entries), allocated on first touch and
`uninitialized_fill`ed with `null` through `allocator_traits`. No `new`.

`fast_mod` is three lines with a `has_single_bit` guard, because the whole scheme
is silently wrong for a non-power-of-two page size and the code should say so at
compile time rather than produce corrupted indices at run time.

`sparse_set_invariants.cpp` pushes ids 0 and 1,000,000 — about 245 pages apart —
and checks that the page in between is never allocated.

## Exercise 3 — measured

**Wall clock**, gcc 13.3 `-O2`, one core, ids spread 4× and shuffled, 16-byte
payload:

| elements | packed iterate | map iterate | ratio | packed lookup | map lookup | ratio |
|---:|---:|---:|---:|---:|---:|---:|
| 10,000 | 0.018 ms | 0.094 ms | **5.3×** | 0.043 ms | 0.153 ms | 3.6× |
| 100,000 | 0.246 ms | 1.068 ms | **4.3×** | 1.733 ms | 7.704 ms | 4.4× |
| 1,000,000 | 3.606 ms | 20.300 ms | **5.6×** | 23.937 ms | 134.496 ms | 5.6× |

**Cache behaviour**, `valgrind --tool=cachegrind --cache-sim=yes
--D1=32768,8,64`, 20,000 elements × 64 passes:

| | D1 read misses | D1 read miss rate | D refs |
|---|---:|---:|---:|
| packed | 422,926 | 7.9% | 8,598,255 |
| map | 1,063,252 | **16.1%** | 9,878,247 |

**In cache lines, which is the explanation the exercise asks for.** One pass over
20,000 elements:

- The packed array is 20,000 × 16 bytes = 320 KB of *contiguous* memory, which is
  320 KB / 64 B = **5,000 cache lines**. Every line that is fetched delivers four
  payloads. Over 64 passes that predicts ≈320,000 D1 read misses; cachegrind
  measured 422,926 including setup. The prediction and the measurement agree,
  which is the point of making the prediction first.
- `std::unordered_map` allocates each node separately. A node holds the key, the
  value and a next pointer, and lands wherever the allocator put it — so a pass
  touches roughly **one line per node, 20,000 of them**. Four times as many lines
  for the same 320 KB of data. Over 64 passes that predicts ≈1.28 M misses;
  cachegrind measured 1,063,252 (some nodes share a line, because the allocator
  is not adversarial).

So the miss ratio is ≈2.5× and the wall-clock ratio is ≈5×. The gap is the second
half of the answer: the map's misses are **dependent** loads. You cannot fetch
node *n+1* until node *n* has arrived, so the misses serialise and the hardware
prefetcher has nothing to predict. The packed array's misses are independent and
strided, so the prefetcher hides most of them. Miss *count* explains 2.5×;
miss *latency exposure* explains the rest.

The random-lookup column is worth keeping too, because it is the operation the
map is supposed to be good at — and it is still 5.6× slower, because a lookup is
`sparse[e / 4096][e % 4096]` (a shift, a mask, two dependent loads) against a
hash, a modulo, a bucket load and a chain walk.

### Note on tooling

`perf stat` is **not usable on this machine**: `perf_event_paranoid` is 4, which
disallows CPU event access without `CAP_PERFMON`. Cachegrind is the substitute
the course itself suggests, and it is arguably the better instrument here — it is
deterministic, so the numbers above reproduce exactly rather than drifting with
system noise. It is also ~50× slower, which is why the cache measurement uses
20,000 elements and the wall-clock one uses a million.

### Measured — 16-core WSL2 (2026-08-27)

`nproc` = 16, i9-9900K, gcc 13.3 `-O2`, Debug build, single-threaded (this
benchmark has no concurrency, so the extra cores buy nothing directly — the
point of re-running it here is faster clock and a different cache hierarchy),
best-of-3, milliseconds:

| elements | packed iterate | map iterate | ratio | packed lookup | map lookup | ratio |
|---:|---:|---:|---:|---:|---:|---:|
| 10,000 | 0.009 | 0.024 | 2.67× | 0.017 | 0.084 | 4.94× |
| 100,000 | 0.091 | 0.265 | 2.91× | 0.286 | 1.100 | 3.85× |
| 1,000,000 | 1.043 | 3.853 | 3.69× | 5.333 | 39.108 | 7.33× |

Against the droplet numbers above: **the two ratios moved in opposite
directions.** Sequential iteration's advantage *shrank* (5.3–5.6× there,
2.7–3.7× here) — a faster core with a bigger, more effective prefetcher
narrows the gap for the map's chain-walk when it is at least walking through
cache lines the prefetcher can chase. Random lookup's advantage *grew* at the
million-element size (5.6× there, 7.33× here) — a lookup's dependent
pointer-chase (hash → bucket → chain link) does not prefetch either way, so it
is exposed to raw memory latency, and the gap between "one dependent load" and
"a chain of them" widens rather than narrows as the core gets faster. Same
code, same asymptotic argument, a genuinely different number depending on
which cost (throughput vs. latency) the faster core actually relieves.

**`perf` is *also* unusable here, but for a different reason than the
droplet's `perf_event_paranoid`.** WSL2 runs a Microsoft-patched kernel
(`6.6.87.2-microsoft-standard-WSL2`) that Ubuntu's `linux-tools-generic`
package does not match — `perf` reports "not found for kernel
6.6.87.2-microsoft" and asks for a `linux-tools-<exact-version>` package that
does not exist in the Ubuntu archive for a Microsoft kernel build.
`perf_event_paranoid` is 2 here (better than the droplet's 4), which would
otherwise have allowed it. This closes the row 7 cross-check the pending-doc
asked for: it is not answerable on WSL2 either, and cachegrind remains the
only instrument that has actually produced numbers in this repo.

## Exercise 4 — the seam

Four private virtuals, called by the base at the points where a slot's contents
move or die:

```cpp
virtual const void *get_at(size_type) const;              // type-erased read
virtual void swap_or_move(size_type lhs, size_type rhs);  // swap two LIVE slots
virtual void move_into(size_type from, size_type to);     // to is VACATED: move-construct
virtual void destroy_at(size_type);                       // a live slot is being vacated
```

The course names the first two. EnTT reaches the same place by making
`pop(first, last)` virtual and overriding the whole loop in the derived class,
which keeps the hook count down but moves more of the algorithm out of the base.
Splitting the cases out keeps *all* the policy logic in the base and leaves the
derived class four one-line hooks, instead of re-implementing erasure once per
policy — where the failure mode is a derived class that forgets one path, and
does so silently.

The split between `swap_or_move` and `move_into` is not bureaucracy, and Module 7
is where it earns itself: after `in_place` erasure the payload slot holds **no
object**, so `compact()` must move-*construct* into it. Swapping into raw storage
is not a thing. A single hook would have had to guess which case it was in, and
the guess would have been wrong exactly when the payload was non-trivial.

For the same reason `swap_and_pop` swaps *before* it destroys: destroying first
would hand the derived class a raw slot to swap with.

Private and virtual is the point: no caller can reach them, and the base must.
The base owns the algorithm (which slot moves where), the derived owns one step
of it (keeping the payload in sync). Non-Virtual Interface, textbook use.

The `static_assert` the exercise asks for:

```cpp
static_assert(std::is_same_v<flat_storage<int>::value_type,
                             flat_storage<std::string>::value_type>);
```

Both instantiations share one base type — which is what makes
`std::vector<base_type *>` over heterogeneous pools possible, and
`virtual_seam.cpp` does exactly that: erases the same entity from an `int` pool
and a `std::string` pool through base pointers, with the base knowing neither.

`in_place`'s `moves() == 0` is checked too: the policy whose entire purpose is
not to move payloads had better not move any.

## Techniques logged

Added to `docs/notes.md`: paged sparse array, `fast_mod`, the intrusive free list
in tombstoned slots, the `swap_only` partition, and the private-virtual seam.
