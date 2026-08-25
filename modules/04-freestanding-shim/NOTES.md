# Module 4 — The freestanding shim

Source under study: `entt/src/entt/stl/`.
Reimplementation: `src/acpp/stl/` (16 headers), plus a replacement at
`modules/04-freestanding-shim/ext/acpp/ext/stl/vector.hpp`.

| Exercise | Deliverable |
|---|---|
| 1 — enumerate EnTT's `std::` surface, write down the count | below; `stl_manifest` pins ours |
| 2 — a fixed-capacity `ext/stl/vector.hpp`, find where EnTT breaks | `entt_ext/`, `entt_on_fixed_vector` |
| 3 — apply the seam to code I own | `src/acpp/stl/`, `seam_default` + `seam_fixed` |

---

## Checkpoint: why `__has_include` beats a build-system flag

A flag has to be **passed**, by everyone, consistently. That means it belongs to
whoever configures the build, not to whoever owns the target — so it travels
badly across a superbuild, a package manager, an IDE, and someone's one-off
reproduction. Miss it in one translation unit and you get an ODR violation, not
an error.

`__has_include` moves the decision to the include path, which the target already
owns and already has to get right for anything to compile at all. Consequences
worth naming:

- **It cannot be half-applied.** The path is the same for every TU in the target,
  so the substitution is consistent by construction.
- **It needs no cooperation from upstream.** No patch, no fork, no `-D` wall, no
  rebase when the pin moves. `third_party/entt` is untouched in this module.
- **It is visible in the build graph.** A `target_include_directories` line is
  greppable; a flag buried in a toolchain file is not.

The cost: the replacement must be found *before* any consumer includes it, and
the header's interface contract is implicit rather than declared. Both showed up
in exercise 2 below.

## Exercise 1 — the dependency manifest

```
$ grep -rho "using std::[a-zA-Z_0-9]*" third_party/entt/src/entt/stl/ | sort -u | wc -l
154
```

**154 distinct names across 21 headers.** The distribution is the interesting
part:

| header | names | | header | names |
|---|---|---|---|---|
| `type_traits.hpp` | 53 | | `tuple.hpp` | 11 |
| `memory.hpp` | 18 | | `concepts.hpp` | 7 |
| `utility.hpp` | 17 | | `functional.hpp` | 6 |
| `iterator.hpp` | 16 | | `algorithm.hpp` | 5 |
| | | | 13 others | 1–4 each |

Was it what I expected? **The total, roughly; the shape, not at all.** Two thirds
of a ~25 kLOC library's entire dependency on the standard library is
`<type_traits>`, `<memory>`, `<utility>` and `<iterator>` — four headers, 104
names, all of them either compile-time machinery or allocation plumbing. The
containers are a rounding error: `vector.hpp` and `string.hpp` re-export exactly
one name each, and `array.hpp` two.

That is the number that changes how you plan a port. The instinct is to worry
about `<vector>`; the actual work is in `<type_traits>`, which is also the part
that is hardest to replace and least likely to need replacing.

`src/acpp/stl/` is the same pattern, 16 headers and **183** names, pinned by the
`stl_manifest` test against `ACPP_STL_MANIFEST_SIZE` in the root CMakeLists.
A manifest whose whole value is that somebody read it must not be able to grow
unnoticed, so adding a dependency means changing a number and seeing it in a
diff. (Ours is larger than EnTT's because it is written out ahead of the modules
that will use it, rather than trimmed to what is used today.)

## Exercise 3 — the seam applied to `acpp`

`seam_swap.cpp` is compiled twice with **no difference but the include path**:

```
seam_default   acpp::stl::vector is std::vector
seam_fixed     acpp::stl::vector is a fixed-capacity, no-heap vector
```

Not one line of `src/` differs, and there is no flag selecting the behaviour.
Both builds are ctest entries, because a seam that silently failed to engage
looks exactly like one that worked — so the test *reports which implementation it
got* rather than assuming.

`compressed_pair.hpp` and `ring_buffer.hpp` were the first files retrofitted, and
they turned up the seam's **floor**: `std::tuple_size` and `std::tuple_element`
stay spelled `std::` because the *language* names them when it expands a
structured binding. Same for `std::initializer_list` and the coroutine traits. A
shim can replace everything a library calls; it cannot replace what the core
language reaches for by name.

## Exercise 2 — EnTT on a fixed-capacity vector

The exercise says: *"You will not get all of EnTT compiling, and finding out
precisely where it breaks is the point."*

**Measured result: all of `entt.hpp` compiles and runs.** `entt_on_fixed_vector`
includes the whole umbrella header, drives a `registry` through creates,
emplaces, a two-component view and a destroy, and then exercises `meta/`'s
runtime reflection — on a vector that cannot allocate.

Getting there was four compile-fix rounds, and the list of what was missing *is*
the result:

| round | what EnTT asked for |
|---|---|
| 1 | typedefs `pointer`, `const_pointer`, `difference_type` |
| 2 | `allocator_type`, a constructor taking one, `get_allocator()`, `resize(n, value)`, `swap`, `shrink_to_fit` |
| 3 | (linked and ran through `registry`, `view`, `any`, `delegate`, `sigh`) |
| 4 | `cbegin` / `cend`, needed only by `meta/` |
| 5 | **the iterator had to stop being a pointer** — see below |

Final interface: **11 typedefs and 20 members**, plus a class-type iterator,
against a `std::vector` that has well over a hundred. That ratio is the honest
headline — the *shape* of `std::vector` a real library depends on is small, and
you cannot find out which small part without doing this.

### The one that only clang found

Rounds 1–4 passed on gcc at both standards. clang rejected it:

```
entt/graph/adjacency_matrix.hpp:25:46: error: type 'const unsigned long *'
    cannot be used prior to '::' because it has no members
```

`edge_iterator::find_next` writes `static_cast<It::difference_type>(pos)`. The
replacement's `iterator` was `Type *`, and a raw pointer has no members. gcc
happened not to instantiate that path; clang did, and clang is right.

This is the sharpest result in the module, because **no amount of reading the
member-function list would have found it**. "Looks like `std::vector`" includes
the iterator being a *class type* carrying the five member typedefs — which is
also why every real `std::vector` implementation wraps its pointer
(libstdc++'s `__normal_iterator`) rather than exposing one. The fix was a
50-line `contiguous_iterator` class, and the general lesson is that a
drop-in replacement's contract includes the *types* it exposes, not only the
operations it supports.

It also re-earns `scripts/verify.sh`: a single-compiler check would have shipped
this.

Two design decisions inside the replacement, both forced by the exercise rather
than chosen:

- **The allocator parameter is accepted and ignored.** Callers hand one over
  because they do not know which vector they got, and refusing it would mean
  editing the library — the thing the seam exists to avoid.
- **Overflow traps rather than reallocating or throwing.** A target that chose a
  heap-free vector wants a loud immediate stop; silently allocating would be the
  worse failure, and an exception may not exist on that target.

### The claim I had to walk back

"Heap-free" was over-claiming, so the test counts global `operator new` calls:

```
global allocations during 32 creates + 48 emplaces + a view pass: 7
```

Swapping the vector removes *the vector's* allocations. It does nothing about
the paged sparse array and paged payload, which allocate through
`allocator_traits` and are a separate seam entirely. Seven trips to the heap
remain.

(The counter is compiled out under ASan and TSan: their runtimes replace the
global allocation functions themselves and the link fails with "multiple
definition of `operator new`". Worth knowing before you reach for that hook in a
sanitized build — and worth *saying so in the output* rather than quietly
reporting zero.)

That is the useful lesson for a real port: `ext/stl/vector.hpp` is necessary and
nowhere near sufficient. The remaining allocations need a replacement
`allocator_type`, which is a different mechanism (a template parameter with a
default, Module 3's `compressed_pair` storing it) reached through a different
door. Two seams, both required, and only one of them is this module's.

## Techniques logged

Added to `docs/notes.md`: the `__has_include` extension seam, the dependency
manifest as an auditable artifact, and where the seam's floor is.
