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
