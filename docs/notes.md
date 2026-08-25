# Technique log

The Module 0 deliverable: one entry per technique — *name, file, one-sentence
mechanism, where I'd use it*. Ordered by module. Per-module written answers live
in `modules/NN-slug/NOTES.md`.

---

## Module 1 — Type identity without RTTI

**Type name from `__PRETTY_FUNCTION__`** · `entt/core/type_info.hpp` ·
`src/acpp/type_info.hpp`
Slice the type's spelling out of the compiler's own signature string, which it
was going to emit anyway, instead of asking `typeid`.
*Use when:* RTTI is off or unaffordable — firmware, `-fno-rtti` builds, or
anywhere `typeid` bloats the binary with type descriptors you never read.

**SFINAE on constant-evaluability** · same file
A defaulted non-type template parameter forces an expression to be
constant-evaluated during deduction; failure is a substitution failure, not an
error, so the overload drops out.
*Use when:* you want "compile-time if possible, runtime if not" and the failure
is *inside* the constant evaluation. `if constexpr` and concepts both fail here —
neither can test constant-evaluability.

**`int`/`char` overload ranking** · same file
Give two otherwise-equal overloads an `int` and a `char` parameter and call with
`0`; exact match beats conversion, deterministically.
*Use when:* you need a fixed preference order between viable overloads without a
`priority_tag<N>` chain. Cheaper to read than partial ordering.

**Compile-time FNV-1a + UDL** · `entt/core/hashed_string.hpp` ·
`src/acpp/hashed_string.hpp`
Fold a string literal to an integer in a `constexpr` function exposed through
`operator""_hs`, so identifiers are readable in source and integral at runtime.
*Use when:* command dispatch, config keys, protocol tags on constrained targets.
Pair it with a `consteval` uniqueness check over the closed set — a collision in
a dispatch table runs the wrong command.

**Cross-DSO static merging** · `entt/config/config.h` ·
`src/acpp/config.hpp`
Default visibility puts a template's function-local static in the dynamic symbol
table as `STB_GNU_UNIQUE`, so the loader keeps one instance process-wide instead
of one per shared object.
*Use when:* any plugin architecture with per-type identity. On GCC the attribute
on the template is not enough — an instantiation's visibility is the minimum over
the template *and its arguments*, so the component types need exporting too.
Measured in `modules/01-type-identity/NOTES.md`.

---

## Module 2 — Traits as an API surface

**Constrained partial specialization** · `entt/entity/component.hpp` ·
`src/acpp/component.hpp`
`template<typename T> requires T::in_place_delete struct in_place_delete<T>:
true_type {};` — a specialization that only exists when the constraint holds,
replacing C++17's `void_t` detection plus two layers of `conditional_t`.
*Use when:* you want a member on the user's type to change a library default.
Note it requires the member to exist *and* be true, so `= false` correctly does
not opt in.

**Policy inferred from type properties** · same file
Derive the default from something the type already tells you, so the common case
needs no configuration at all.
*Use when:* the inference is a *correctness* statement, not a guess.
`in_place_delete = !movable` qualifies because swap-and-pop is not an available
implementation for a non-movable type. "Probably wants X" does not qualify — that
belongs on the opt-in rung.

**Customization-point ladder** · same file
Three rungs: inferred default, inline `static constexpr` opt-in, full trait
specialization. Each more invasive than the last.
*Use when:* designing any library API surface. The middle rung is what separates
a library from a framework — the user opts in from inside their own type, with no
macro and no specialization in your namespace.

**Branch collapsed into arithmetic** · same file
`!is_empty_v<T> * PACKED_PAGE` instead of a ternary, so the empty case falls out
as `0` and every downstream user gets "allocate nothing" without an `if`.

**Type list algebra** · `entt/core/type_traits.hpp` · `src/acpp/type_traits.hpp`
A `type_list<T...>` with no storage and no members, manipulated purely by partial
specialization. Costs instantiations, nothing else.
*Use when:* you need a compile-time sequence of types. Not `std::tuple`, which
drags storage and a large header along.

**Fold-expression accumulator instead of recursion** · `src/acpp/type_traits.hpp`
Carry state through a left fold over a declared-but-undefined `operator+`, named
only inside `decltype`, rather than recursing through partial specializations.
*Use when:* instantiation depth matters — and it usually does, because depth is a
cliff (`-ftemplate-depth` is a hard build failure) where time is a slope.
Measured in `modules/02-traits/NOTES.md`.

**Policy as a trait, not a macro** · `src/acpp/counter.hpp`
Replace `#if`-driven policy with a trait keyed on a tag type: inferred default,
inline opt-in, specialization override.
*Use when:* a build flag is answering a question that different callers in the
same build could legitimately answer differently.

---

## Module 3 — Layout economy

**EBO with a disambiguating tag** · `entt/core/compressed_pair.hpp` ·
`src/acpp/compressed_pair.hpp`
A primary template that stores by value and a `requires is_ebco_eligible_v<Type>`
specialization that inherits instead; a `size_t Tag` parameter makes two bases of
the same empty type distinct types so the derivation is legal.
*Use when:* storing a stateless allocator, deleter, comparator or hash.
*Know that:* the tag buys legality, not size — two subobjects of the same type
still need distinct addresses, so `pair<empty, empty>` is 2 bytes while
`pair<empty, other_empty>` is 1. Measured in `modules/03-layout-economy/NOTES.md`.

**`is_ebco_eligible_v`, not `is_empty_v`** · `entt/core/type_traits.hpp`
An empty *final* class is still empty but cannot be inherited from.
*Use when:* any time you are about to test `is_empty_v` in order to inherit.

**`[[no_unique_address]]` as the alternative** · `src/acpp/compressed_pair.hpp`
Far less code, and it compresses empty `final` types the inheritance version
cannot. MSVC's ABI ignores the plain attribute and needs
`[[msvc::no_unique_address]]`, which is why libraries still ship both.

**Layout `static_assert` battery** · `modules/03-layout-economy/layout_assertions.cpp`
State every size, alignment, `offsetof` and standard-layout assumption the code
actually depends on, once, in code.
*Use when:* always — it is what catches ABI drift when the toolchain moves.
*Do not:* assert facts nothing depends on (`sizeof(void *)`), or the battery
becomes something people delete instead of read.

---

## Module 4 — The freestanding shim

**`__has_include` extension seam** · `entt/stl/*.hpp` · `src/acpp/stl/`
One file per standard header, each an explicit `using` re-export guarded by
`#if __has_include(<lib/ext/stl/x.hpp>)`. The library then spells everything
`stl::` internally.
*Use when:* you may ever need to port onto a toolchain with a partial, ancient or
exception-free standard library.
*Why not a build flag:* a flag must be passed by everyone consistently and misses
become ODR violations rather than errors; an include path is owned by the target
and cannot be half-applied.
*Floor:* `std::tuple_size`, `std::tuple_element`, `std::initializer_list` and the
coroutine traits stay `std::` — the core language names them.

**The `using` list as a dependency manifest** · same files
The re-export list is the complete, auditable inventory of what a library needs
from the standard library. EnTT's is 154 names across 21 headers, and two thirds
of it is `<type_traits>`, `<memory>`, `<utility>` and `<iterator>`.
*Use when:* planning a port, or reviewing what a dependency actually costs. Pin
the count in CI (`cmake/check_manifest.cmake`) so it cannot grow unread.

**Fixed-capacity container behind a standard-shaped interface** ·
`modules/04-freestanding-shim/ext/acpp/ext/stl/vector.hpp`
Implement only the subset the consumer uses; accept and ignore the allocator;
trap on overflow rather than reallocate.
*Measured:* all of EnTT compiles and runs against 11 typedefs and 20 members.
Swapping the vector still leaves 7 heap allocations per registry workload,
because the paged arrays allocate through `allocator_traits` — a second seam.

---

## Module 5 — Bit-packed handles

**Generational index handles** · `entt/entity/entity.hpp` · `src/acpp/handle.hpp`
Pack an index and a generation counter into one integer; bump the generation on
release so a stale handle fails validation with one compare.
*Use when:* you need stable, cheap, copyable references into a dense array.
Raw indices dangle silently, pointers dangle on reallocation, `shared_ptr` costs
an atomic and a cache miss per dereference.
*Sizing:* size the version for the *worst-case* slot, not the average — churn is
never uniform, and one hot slot absorbs a large fraction of it. Arithmetic in
`modules/05-handles/NOTES.md`.

**Trait inheritance via `requires requires`** · same file
A nested `requires requires { requires is_enum_v<T>; typename traits<underlying_t<T>>::value_type; }`
lets `enum class my_id : uint32_t {};` be a first-class handle type with nothing
else written, while staying a constraint rather than a hard error for enums
whose underlying type has no traits.

**Shift derived from the mask** · same file
`popcount(index_mask)` instead of a second constant, so changing the split is a
one-line edit and the two can never disagree.

**Sentinels that compare on one field** · same file
`null` and `tombstone` share a bit pattern; `null::operator==` compares the index
part, `tombstone::operator==` the version part.
*Why:* a tombstoned slot must be able to carry *another* slot's index in its
index bits and still read as a hole — which is what makes Module 6's intrusive
free list possible.

**Branchless reserved-value skip** · same file
`version + (version == version_mask)` steps over the reserved all-ones
generation without a branch.

**Lazy trait dispatch instead of `conditional_t`** · same file
`conditional_t<is_enum_v<T>, underlying_type_t<T>, T>` is a hard error for
non-enums: both branches are evaluated. A constrained partial specialization
(`underlying_or_self`) is the fix.

---

## Module 6 — The sparse set

**Paged sparse array** · `entt/entity/sparse_set.hpp` · `src/acpp/sparse_set.hpp`
The sparse array is a vector of fixed-size pages allocated on first touch, not
one flat array.
*Why:* it is indexed by entity *id*, which can be arbitrarily spread out. A flat
array sized to the largest id wastes memory catastrophically; paging bounds the
waste to one page.
*Note:* this is a **memory** argument. Module 7 pages the *payload* for a
completely different reason (pointer stability), and conflating the two is the
easiest mistake to make here.

**`fast_mod` with a power-of-two assert** · `entt/core/bit.hpp` · `src/acpp/bit.hpp`
`value & (mod - 1)`, with `has_single_bit(mod)` checked at compile time.
*Use when:* any time a size is a power of two by construction — say so in the
code, because the arithmetic is silently wrong otherwise.

**Intrusive free list in tombstoned slots** · `entt/entity/sparse_set.hpp`
A freed slot stores the index of the previously freed slot in its entity bits and
the tombstone in its version bits; `head` points at the most recent hole.
*Use when:* you need a free list over fixed-size slots and the slots are big
enough to hold an index. Costs zero extra memory.
*Depends on:* a sentinel that compares on one field only (Module 5) — otherwise a
slot cannot both carry an index and read as dead.

**Partitioned array as a free list (`swap_only`)** · same file
Partition at `head`: live below, released above. Release swaps up, recycle swaps
down.
*Use when:* elements are never really destroyed, only deactivated — sessions,
entity ids, slot-based resources. Recycling needs no separate structure at all.

**Branchless policy dispatch** · same file
`max_size * (mode != swap_only)` instead of a ternary, because the two things
`head` denotes are complementary.

**Private-virtual (NVI) seam** · same file
The base owns the algorithm and calls private virtuals for the one step it cannot
know; the derived class implements them.
*Use when:* a type-erased base must reorder elements whose payload type it does
not know. `virtual_seam.cpp` erases from an `int` pool and a `string` pool
through the same base pointer.

**Contiguous iteration beats node-based, in cache lines** ·
`modules/06-sparse-set/NOTES.md`
Measured: 5.6× wall clock over `std::unordered_map` at 1M elements, from 2.5×
fewer D1 read misses (cachegrind) plus the fact that the map's misses are
*dependent* loads the prefetcher cannot hide.
