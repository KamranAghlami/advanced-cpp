# Module 1 — Type identity without RTTI

Source under study: `entt/core/type_info.hpp`, `core/hashed_string.hpp`,
`config/config.h`. Reimplementation: `src/acpp/{config,hashed_string,type_info}.hpp`.

| Exercise | Deliverable |
|---|---|
| 1 — `stripped_type_name` from scratch | `src/acpp/type_info.hpp`, proved by `type_name_probe` + four `codegen_*` tests |
| 2 — constexpr string→ID mapper | `cmd_dispatch`, `codegen_cmd_dispatch*` |
| 3 — cross-DSO ODR failure and fix | `odr_shared.hpp`, `odr_plugin.cpp`, `odr_across_dso` |
| 4 — `int`/`char` ranking applied elsewhere | `constexpr_to_string` |

---

## Checkpoint: why does `type_name<T>(0)` pick one overload over the other?

Two independent mechanisms, both required.

**The `auto =` parameter selects.** The preferred overload is

```cpp
template<typename Type, auto = stripped_type_name<Type>().find_first_of('.')>
ACPP_CONSTEVAL std::string_view type_name(int) noexcept;
```

A non-type template parameter with a default *must* be constant-evaluated during
template argument deduction. If `stripped_type_name<Type>()` cannot produce a
constant — the compiler spelled the type in a way the slice cannot handle, and
`substr` throws on a bad index — that is a substitution failure in the immediate
context, so the candidate silently drops out of the overload set.

`if constexpr` cannot do this job. Its condition has to be a constant expression
that *evaluates* successfully; here the failure happens inside the evaluation
itself, so there is nothing to test.

A concept cannot do it either: `requires { stripped_type_name<Type>(); }` is
satisfied by any expression that is merely well-formed. Constant-*evaluability*
is not expressible as a requirement.

**The `int`/`char` parameter ranks.** When both candidates survive, the call site
passes the literal `0`. That is an exact match for `int` and an integral
conversion for `char`, so the `int` overload wins by ordinary ranking. No partial
ordering rules, no tag types, no `priority_tag<N>` chain.

**What breaks without the `auto =` parameter:** the consteval overload becomes
unconditionally viable, so every type takes it. Types whose names cannot be
extracted at compile time turn into hard errors at the point of use instead of
falling back to the runtime path. The fallback overload becomes dead code.

## Exercise 1 — codegen

`-O2`, gcc 13.3, x86-64, checked in CI by `cmake/check_asm.cmake`:

| Probe | Body | Content |
|---|---|---|
| `acpp_probe_name_data` | 3 | `endbr64`, `leaq 69+.LC0(%rip), %rax`, `ret` |
| `acpp_probe_name_size` | 3 | `endbr64`, `movl $16, %eax`, `ret` |
| `acpp_probe_hash` | 3 | `endbr64`, `movl $-259785546, %eax`, `ret` |
| `acpp_probe_index` | 24 | guard load, branch, `__cxa_guard_acquire`, `call next()` |

(`endbr64` is CET landing-pad padding from Ubuntu's default
`-fcf-protection`, not work. `69+.LC0` is an offset *into* the signature string
the compiler emitted for `__PRETTY_FUNCTION__` — no separate copy of the name is
stored, which is the point. `16` is `strlen("std::vector<int>")`.)

The first three are the claim ("zero runtime instructions") made concrete: the
`string_view` is two immediates into a string the compiler was emitting anyway.

The fourth is the control, and it is the reason to trust the other three. The
sequential ID genuinely cannot be folded — it depends on the first-touch order of
every type in the process — so its probe must show the thread-safe-static guard.
`codegen_type_index_is_not_free` asserts that it does. Without a case that is
required to fail the "is it free?" test, the passing cases prove nothing about
the harness.

## Exercise 2 — dispatch codegen

`acpp_probe_dispatch` compiles to integer compares against immediates with no
call. The check that matters is the negative one: `strcmp`, `memcmp` and
`strncmp` are forbidden in the body. Their presence would mean the hash was not
folded at compile time and the "zero overhead identifier" is a string compare
wearing a hat.

`acpp_probe_dispatch_constant`, where the command is a literal at the call site,
is `mov $3, %eax; ret`.

**Collision handling.** `cmd_dispatch.cpp` carries a `consteval all_distinct()`
over the closed command set, asserted at compile time. FNV-1a over 32 bits has a
~50% chance of some collision at roughly 77k distinct strings; a dispatch table
has maybe a hundred, so the practical risk is small — but "small" is not a
decision. The cost of a collision here is *the wrong command executes*, which is
not absorbable, and the set is closed and known at compile time, so the check is
free. Copy the hashing pattern only together with a check of this shape.

## Exercise 3 — the cross-DSO failure, measured

The naive header-only `type_index` gives `beta` id **1** in the host and **2** in
the plugin. The fixed one gives **3** in both. Run `odr_across_dso` to see it.

The mechanism, from `readelf -sW`:

```
$ readelf -sW build/modules/01-type-identity/CMakeFiles/odr_plugin.dir/odr_plugin.cpp.o | c++filt
  OBJECT UNIQUE HIDDEN   odr::naive::type_index<odr::beta>::value()::value
  OBJECT UNIQUE DEFAULT  acpp::type_index<odr::beta>::value()::value
```

`UNIQUE` is `STB_GNU_UNIQUE`: GCC's binding for a function-local static inside an
inline function or template, which asks the loader to keep exactly one instance
process-wide. It is the merge mechanism, and it only takes effect for symbols
that reach the dynamic symbol table — that is, `DEFAULT` visibility ones. A
`HIDDEN` unique symbol is unique per shared object, which is precisely the bug.

**Measured, and not what the course text implies:** on GCC, the visibility
attribute on the class template is *not sufficient on its own*.

> The visibility of a template instantiation is the minimum of the visibility of
> the template and the visibility of its arguments.

So `acpp::type_index<Type>` with `ACPP_API` still comes out `HIDDEN` when `Type`
was compiled under `-fvisibility=hidden` and carries no attribute of its own.
That is why `odr::alpha`, `odr::beta` and friends are marked `ACPP_EXPORT` in
`odr_shared.hpp`. Verified by flipping the attribute off and re-running
`readelf`; without it the "fixed" case fails exactly like the naive one.

The practical consequence for a plugin architecture: exporting the registry's
templates is not enough. Every type whose identity crosses the boundary has to be
visible too — which in a real codebase means the shared header that declares the
component types needs the export macro on each of them.

Also worth noting: the *function* `acpp::type_index<beta>::value()` stays
`HIDDEN` under `-fvisibility-inlines-hidden` even in the working case. Only the
static inside it merges. So comparing function addresses across the boundary is
the wrong diagnostic; compare the IDs, which is what the test does.

### Windows has no version of this mechanism

`odr_across_dso` is not built on Windows, and that is a finding rather than a
gap. The whole exercise turns on weak-symbol **coalescing**: ELF and Mach-O both
emit a template's function-local static in every image that instantiates it and
then have the loader collapse those definitions onto one, with visibility
deciding whether it may. Windows does no such thing. A DLL and an EXE each keep
their own copy unconditionally, and no attribute changes that --
`__declspec(dllexport)` on both sides yields two exported copies, not one shared
one.

So on Windows the *bug* half of this exercise is unavoidable and the *fix* half
does not exist. The real Windows answer is architectural: put the counter in one
DLL and give everyone else `__declspec(dllimport)`, i.e. make the sharing
explicit in the build graph rather than asking the loader to infer it. That is a
different lesson, and a good argument for `type_hash` over `type_index` being
even stronger there.

**And the conclusion the exercise is really for:** `type_hash` needed none of
this. It is a pure function of the type's spelling, computed independently on
both sides and equal by construction. Sequential IDs are for dense array indices
inside one binary. The moment an identifier crosses a boundary — a DSO, a socket,
a file — you wanted the hash.

### Re-measured on Mach-O — Apple clang 21, arm64, libc++ (2026-08-26)

The ELF story above is complete for ELF. On macOS the same source, same flags and
same test **failed**, and the reason turned out to be a sharper version of the
same lesson.

First result, before any change:

```
beta: naive host=1 plugin=2 | fixed host=1 plugin=2
FAIL  the id agrees across the boundary once both are visible
```

The fixed case behaved exactly like the naive one. `nm -m` says why — this is the
Mach-O counterpart of the `readelf` output above:

```
$ nm -m libodr_plugin.dylib | c++filt
  non-external (was a private external)  acpp::type_index<odr::beta>::value()::value
```

There is no `STB_GNU_UNIQUE` on Mach-O. The merge mechanism is dyld's coalescing
of **weak external** definitions across images, and `weak external` is a linkage
class the static local has to actually *be* in. `-fvisibility-inlines-hidden`
demotes it to `non-external`, so each image kept its own copy and nothing
coalesced.

On ELF that same flag is harmless — the note above records that the *function*
stays `HIDDEN` while the static still merges, because `UNIQUE`/`DEFAULT` is
decided separately from the enclosing function's visibility. On Mach-O there is
no such separation: hiding the inline hides the static, and the static is the
whole fix. **One flag, free on one platform and fatal on the other.**

`VISIBILITY_INLINES_HIDDEN` is therefore `OFF` on Apple (root `CMakeLists.txt`,
`ACPP_INLINES_HIDDEN`). The bug reproduction survives untouched, because
`naive::type_index` is hidden by an explicit `ACPP_HIDDEN` attribute rather than
by the flag — visible in the same `nm` dump, one line apart:

```
  non-external ... odr::naive::type_index<odr::beta>::value()::value   <- still split
  weak external ... acpp::type_index<odr::beta>::value()::value        <- now merged
```

#### The part that would have been missed

Fixing only the two Module 1 targets made the test pass — and left something
worse than the original bug in place:

```
beta: naive host=1 plugin=2 | fixed host=1 plugin=1     <- test PASSES
```

`1`, where ELF gives `3`. The per-type memoised static was coalescing, but
`acpp::sequential_counter<>::next()::value` — inside the `acpp` static library,
still compiled with the flag ON — was not. Two counters, both starting at zero,
each handing out dense ids to a different set of types. Measured with a probe
against the real headers (`-fvisibility-inlines-hidden` on the `acpp` TU only):

```
host alpha=0 beta=1 | plugin-private a=0
cross-boundary agreement for beta: OK
uniqueness across types: *** COLLISION ***
```

`alpha` and a type the plugin registered at load time both hold id **0**. Since
these ids exist to index storage arrays directly, that aliases two component
types onto one slot — silently, with the exercise's own test green, because
agreement and uniqueness are different properties and only the first was checked.

With the flag off for `acpp` too, the counter is `weak external`, dyld coalesces
it, and the numbers become `fixed host=3 plugin=3` — identical to ELF and to the
ordering predicted at the top of `odr_across_dso.cpp`.

**The lesson, restated:** a cross-DSO identity scheme has two invariants, not
one. *Same type → same id* is what the test checked. *Different types → different
ids* is the one that was broken, and it needs every static in the chain to merge,
not just the one the test happens to look at. The exercise's real conclusion is
unchanged and now doubly earned: `type_hash` needed none of this on either
platform.

## Exercise 4 — the ranking trick, relocated

`constexpr_to_string.cpp` applies the same shape to a different problem:
`to_string(T)` takes an allocation-free `fixed_string` path when `T` can be
formatted during constant evaluation, and an `ostringstream` path when it cannot.
The probe is `auto = stringify(Type{})`.

Two design points fell out that are worth keeping:

- The two paths return **different types** (`fixed_string<24>` and `std::string`),
  and that is a feature. A caller on the constexpr path should not be handed a
  `std::string` it did not need, and the return type is how a caller can tell
  which path it got. `static_assert(std::is_same_v<decltype(to_string(point{})),
  std::string>)` is a real test.
- Integer formatting accumulates through `std::make_unsigned_t<Type>` so that
  `INT_MIN` does not overflow on negation. `static_assert(to_string(int8_t{-128})
  == "-128")` pins it.

## Techniques logged

Added to `docs/notes.md`: pretty-function name extraction, SFINAE on
constant-evaluability, `int`/`char` overload ranking, compile-time FNV-1a with a
UDL, cross-DSO static merging.
