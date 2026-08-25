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
