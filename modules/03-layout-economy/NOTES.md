# Module 3 — Layout economy

Source under study: `entt/core/compressed_pair.hpp`.
Reimplementation: `src/acpp/compressed_pair.hpp`, `src/acpp/ring_buffer.hpp`.

| Exercise | Deliverable |
|---|---|
| 1 — `compressed_pair` + a `[[no_unique_address]]` version, sizes compared | `compressed_pair_layout.cpp` |
| 2 — an existing container made allocator-aware | `src/acpp/ring_buffer.hpp`, `allocator_aware_ring.cpp` |
| 3 — a `static_assert` battery | `layout_assertions.cpp` |
| checkpoint | `tagless_pair_fails.cpp` + `compile_fail_tagless_pair` |

---

## Checkpoint: what the `Tag` parameter is for, and the exact code that breaks without it

`compressed_pair_element<Type, size_t Tag>` carries a `size_t` that nothing ever
reads. Its only job is to make `compressed_pair_element<empty, 0u>` and
`compressed_pair_element<empty, 1u>` **different types**, so that

```cpp
class compressed_pair: compressed_pair_element<First, 0u>,
                       compressed_pair_element<Second, 1u> { ... };
```

has two distinct direct bases when `First` and `Second` are the same empty type.

Without it, `tagless_pair<empty, empty>` names `tagless_element<empty>` twice as
a direct base, and a class may not do that. `tagless_pair_fails.cpp` is that
code, and `compile_fail_tagless_pair` requires the build to reject it:

```
gcc:   error: duplicate base type 'tagless_element<empty>' invalid
clang: error: base class 'tagless_element<empty>' specified more than once
              as a direct base class
```

What the tag does **not** buy is size. See the measured table below: it makes
the same-type case legal, not free.

## Exercise 1 — measured sizes

gcc 13.3 and clang 18.1.3, x86-64, both standards. `sizeof(int) == 4`.

| | `compressed_pair` | `nua_pair` (`[[no_unique_address]]`) |
|---|---|---|
| `<empty, int>` | 4 | 4 |
| `<int, empty>` | 4 | 4 |
| `<empty, also_empty>` | 1 | 1 |
| `<empty, empty>` | **2** | **2** |
| `<stateful, int>` | 8 | 8 |
| `<final_empty, int>` | **8** | **4** |
| `<allocator<int>, int *>` | 8 | 8 |
| `<empty, over_aligned(32)>` | 32 | — |

gcc and clang agree on every row.

Three results worth keeping.

**Two halves of the *same* empty type cost 2, not 1.** This is the one I got
wrong on the first pass, and it is the more interesting half of the Tag story.
The tag makes `compressed_pair<empty, empty>` *compile*; it does not make it
free. Distinct objects of the same type must have distinct addresses, so the
second base lands at offset 1 and the pair is two bytes. `<empty, also_empty>` —
two different empty types — really is one byte. Compression is per *type*, not
per empty member, and `[[no_unique_address]]` obeys exactly the same rule.

The address rule behind that row is checked directly rather than inferred:
`&first() == &second()` for `pair<empty, also_empty>` (different types, sharing
an address is allowed and happens), and `!=` for `pair<empty, empty>`.

**`final` breaks the inheritance version and not the attribute version.** An
empty `final` class is still empty, so `std::is_empty_v` says yes — but you
cannot derive from it, so EBO cannot apply. That is why `is_ebco_eligible_v` asks
`is_empty_v && !is_final_v` rather than just `is_empty_v`, and it is a genuine
capability advantage of `[[no_unique_address]]`: there is no base class involved,
so finality is irrelevant.

**MSVC.** The course asks for it and this machine has none. As of 2026-08-26 CI
has an `msvc / C++20` and `msvc / C++23` job, so `compressed_pair_layout` now
runs there and the numbers come from a real compiler rather than from the
manual.

The claim being tested: MSVC's ABI ignores plain `[[no_unique_address]]` for
backward compatibility and requires `[[msvc::no_unique_address]]` to compress at
all — which is the reason EnTT still ships the inheritance-based
`compressed_pair` rather than deleting it in favour of the attribute. If that
holds, `nua_pair<empty, int>` is **8** on MSVC where it is 4 on libstdc++ and
libc++, and the inheritance version is 4 on all three.

Fill this in from the first green MSVC run. Until then it is documented
behaviour, not a measurement, and `compressed_pair.hpp` says so.

## Exercise 2 — allocator-aware without a stateless tax

`ring_buffer<Type, Allocator>` stores `compressed_pair<Allocator, pointer>`.

| container | size |
|---|---|
| hand-written, allocator hard-coded away | 32 |
| `ring_buffer<int>` (`std::allocator`) | **32** |
| `ring_buffer<int, counting_allocator>` | 40 |

So: zero for the stateless case, and exactly `sizeof(the allocator)` for the
stateful one — no padding surprise on either side. Both are `static_assert`s in
`allocator_aware_ring.cpp`, not just printed numbers, so a layout regression is a
build failure.

Two design points in the ring itself that are not about allocators but are worth
recording:

- **Capacity rounds up to a power of two** so wrapping is `counter & mask`
  rather than `counter % capacity`. The same trick `fast_mod` uses in Module 6.
- **`head` and `tail` are free-running counters**, never wrapped. `size()` is
  `tail - head`, which makes "full" distinguishable from "empty" without wasting
  a slot and without a separate count. Unsigned wraparound of the counters
  themselves is well-defined and the subtraction stays correct across it.

## Exercise 3 — the battery, and what to leave out

`layout_assertions.cpp` states the assumptions this project actually depends on:
`CHAR_BIT == 8`, `sizeof(id_type) == 4`, both page sizes powers of two, the
compressed and uncompressed pair sizes, `ring_buffer`'s four-word layout, and a
wire-format struct's `offsetof`s.

The rule that keeps it useful is what it *does not* assert. `sizeof(void *)` is
deliberately absent: nothing here depends on the pointer width, and asserting it
would break a 32-bit port over a fact nobody needed. A battery that pins
everything true is a battery people delete.

The `offsetof` block also documents a real trap. `wire_header` is
`{u32, u16, u16, u64}`, and `length` sits at **8**, not 10 — two bytes of padding
after `flags` so the `u64` is aligned. Writing the assertion is how you discover
that before the wire format does. Note `offsetof` is only defined for
standard-layout types, which is exactly why `compressed_pair` — which puts data
in a base — is *not* one, and the battery says so rather than letting someone
reach for `offsetof` on it.

## 3.3 — where empty types disappear entirely

Not an exercise, but the thread to keep hold of: Module 2's
`page_size == 0` for empty types means Module 7's storage allocates no payload
array at all, and `get_as_tuple()` returns an empty tuple so that `tuple_cat` in
the view iterator just skips it. Nothing downstream has a special case. Picked up
in `modules/07-storage-and-views`.

## Techniques logged

Added to `docs/notes.md`: EBO with a disambiguating tag, `[[no_unique_address]]`
and where it differs, allocator storage compression, layout `static_assert`
batteries.
