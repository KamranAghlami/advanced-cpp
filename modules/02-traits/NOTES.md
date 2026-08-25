# Module 2 — Traits as an API surface

Source under study: `entt/entity/component.hpp` (59 lines), `entt/core/type_traits.hpp`.
Reimplementation: `src/acpp/component.hpp`, `src/acpp/type_traits.hpp`, `src/acpp/counter.hpp`.

| Exercise | Deliverable |
|---|---|
| 1 — `serialization_traits<T>`, three-level ladder | `serialization_traits.cpp` |
| 2 — `type_list_unique` twice, measured | `type_list_algebra.cpp`, `type_list_depth_probe.cpp`, `type_list_unique_cost` |
| 3 — a macro policy converted to a trait | `src/acpp/counter.hpp`, `counter_traits.cpp` |
| checkpoint | `component_traits_ladder.cpp` |

---

## Checkpoint: why is `in_place_delete` defaulting to the inverse of movability a *correctness* decision?

Because swap-and-pop is not an *available implementation* for a non-movable type.

Erasing under `swap_and_pop` means "move the last element into the hole and
shrink". A type that is not move constructible and move assignable cannot be
moved there. So the default is not a guess about what the user probably wants —
it is the only policy that compiles.

The performance question sits one rung up, and it points the other way: a
perfectly movable type may still need `in_place` because something outside the
container holds pointers into it. Nothing about the type can reveal that, which
is exactly why level 2 exists and why level 1 must not try to infer it.
`component_traits_ladder.cpp` pins both halves:

```cpp
template<typename Type>
constexpr bool inference_is_sound =
    traits<Type>::in_place_delete
    || (std::is_move_constructible_v<Type> && std::is_move_assignable_v<Type>);
```

Wherever the inference says "swap-and-pop", the operations swap-and-pop needs
are actually there.

## The ladder

Three rungs, each more invasive than the last:

| | How | Who writes it | Cost |
|---|---|---|---|
| 1 | inferred from the type | nobody | zero |
| 2 | `static constexpr` member | the component's author, inline | one line, in your own type |
| 3 | specialize `component_traits` | anyone, from outside | a specialization in someone else's namespace |

Level 2 is what makes this a library rather than a framework. The user opts in
from inside their own type — no macro, no specialization in the library's
namespace, no registration call. In C++17 the detection behind it would have been
`void_t` plus a two-layer `std::conditional`; in C++20 it is a constrained
partial specialization that reads as a sentence:

```cpp
template<typename Type>
    requires Type::in_place_delete
struct in_place_delete<Type>: std::true_type {};
```

Note the constraint is `Type::in_place_delete`, not
`requires { Type::in_place_delete; }` — the member must exist *and* be
contextually convertible to `true`, so `= false` correctly does not opt in.

**The multiplication idiom.** `page_size` is
`!is_empty_v<ACPP_ETO_TYPE(Type)> * ACPP_PACKED_PAGE`. A branch collapsed into
arithmetic at compile time, and — more usefully — the empty case falls out as a
plain `0` rather than as a special case everything downstream has to know about.
Module 7's storage allocates `page_size` slots and therefore allocates nothing
for a tag component without a single `if`.

**One thing I got backwards on the first pass**, worth recording because the
polarity is not obvious: `ENTT_ETO_TYPE(Type)` substitutes `Type` when the empty
type optimisation is *enabled* and `void` when `ENTT_NO_ETO` turns it off.
`is_empty_v<void>` is false, so substituting `void` makes every type look
non-empty and every component get a real page. Reading the macro as "the type ETO
considers" rather than "the type" is what makes it click.

## Exercise 2 — two implementations of `type_list_unique`

Both keep the first occurrence and preserve order, and both agree with
`entt::type_list_unique_t` on every case in `type_list_algebra.cpp` (checked by
converting our list into `entt::type_list` and comparing).

**Recursive** (EnTT's shape) walks one element at a time, carrying the result as
a trailing pack:

```cpp
template<typename First, typename... Other, typename... Kept>
struct type_list_unique_recursive<type_list<First, Other...>, Kept...>
    : std::conditional_t<(std::is_same_v<First, Kept> || ...),
                         type_list_unique_recursive<type_list<Other...>, Kept...>,
                         type_list_unique_recursive<type_list<Other...>, Kept..., First>> {};
```

Each step's type depends on the next step's, so the instantiations nest.

**Folded** carries the accumulator through a left fold over a declared-but-never-
defined `operator+`, named only inside `decltype`:

```cpp
using type = decltype((unique_fold<type_list<>>{} + ... + std::type_identity<Type>{}))::type;
```

One `push_back_unique` instantiation per element, and the fold itself does not
nest.

### Measured

`ctest --test-dir build -L measurement` runs `type_list_unique_cost`, which
binary-searches the smallest `-ftemplate-depth` each variant still compiles at
and times four compiles of each. Input: 64 distinct types each appearing twice
(a 128-element list), unique answer 64. gcc 13.3, `-std=c++23`:

| variant | minimum `-ftemplate-depth` | 4 compiles |
|---|---|---|
| recursive (ours) | **132** | 1 s |
| folded (ours) | **7** | 1 s |
| `entt::type_list_unique_t` | **133** | 3 s |

Depth ~= list length for the recursive formulation, as expected: each step's type
is defined in terms of the next, so the instantiations nest one per element.
EnTT's 133 confirms ours is the same shape rather than an unfair strawman. The
folded one is **7** and does not move with the list length — the fold expression
is not recursive, so the only depth is the fixed nesting of
`push_back_unique` -> `conditional_t` -> `is_same_v`.

The times are the weakest number here and should not be over-read: CMake's clock
has one-second resolution, this machine has one core, and the EnTT column also
pays for a much larger header. Depth is the number worth trusting, because it was
measured by bisection on a hard yes/no answer rather than by a timer.

The test asserts the *relationship* (recursive deeper than folded) rather than
absolute numbers, so it stays meaningful on another compiler.

`type_list_unique_t` — what the rest of the project uses — is the folded one.
Not because it is dramatically faster on a 64-type list, but because
instantiation depth is a cliff, not a slope: exceed `-ftemplate-depth` and the
build fails outright with an error that points at the library rather than at the
list. Flat depth means the failure mode is gone rather than deferred.

### The tool that was supposed to do this

The course specifies clang's `-ftime-trace`. `docs/CLAUDE.md` recorded clang as
not installed on this machine; **it is now** (`clang 18.1.3`, installed while
fixing a CI failure this module turned up — see below). The depth measurement
stayed anyway, because it answers the question more directly: `-ftime-trace`
shows where time went, not how deep the instantiation stack got, and depth is the
number that turns into a hard build failure.

## Exercise 3 — the macro this repo actually had

Module 1 introduced a real policy macro:

```cpp
#if defined ACPP_NO_ATOMIC
#    define ACPP_MAYBE_ATOMIC(Type) Type
#else
#    define ACPP_MAYBE_ATOMIC(Type) std::atomic<Type>
#endif
```

Three problems, all generic to policy macros: one answer for the entire build, no
visibility at the point of use, and no way for a caller to disagree without
recompiling everything.

`src/acpp/counter.hpp` replaces it with `counter_traits<Tag>`:

- **inferred** from `__STDCPP_THREADS__`, which is the standard's own answer to
  "can this program have more than one thread of execution" — the right thing to
  key on, and free on a freestanding single-threaded target;
- **opt in** with `static constexpr bool atomic_counter = false;` on the tag,
  which is per counter rather than per build — the rung the macro could never
  offer;
- **override** by specializing `counter_traits`.

`ACPP_NO_ATOMIC` survives, but only as a way to move the default. That is what a
build flag should have been doing in the first place.

`type_index.cpp` now reads `sequential_counter<type_index>::next()`.
`counter_traits.cpp` checks all three rungs and then hammers the default counter
from four threads, verifying that 8000 concurrent draws produce every id exactly
once (sum and xor of the multiset both have to match `0..7999`).

## Two bugs this module surfaced

**A dangling temporary in Module 1's `constexpr_to_string.cpp`**, found because
CI's clang legs failed after the Module 1 push:

```cpp
for(const char c: stringify(minor).view())   // dangles
```

Before C++23 (P2718R0) only the range initializer's top-level temporary is
lifetime-extended, and here that temporary is the `string_view`, not the
`fixed_string` it points into. GCC at `-std=c++23` accepted it; clang rejected it
at both standards, correctly. Fixed by binding the `fixed_string` to a named
local. The lesson is not about ranges — it is that a single-compiler,
single-standard local build is not a check.

Hence `scripts/verify.sh`, which runs the CI matrix (gcc/clang × C++20/23) plus
the ASan/UBSan and TSan legs locally before a push.

**The `ETO_TYPE` polarity** described above, caught by a `static_assert` in
`component_traits_ladder.cpp` rather than by anything at runtime — which is the
argument for the `static_assert` battery habit the course pushes in Modules 3
and 7.

## Techniques logged

Added to `docs/notes.md`: constrained partial specialization, policy inferred
from type properties, the customization-point ladder, type list algebra,
fold-vs-recursion instantiation cost.
