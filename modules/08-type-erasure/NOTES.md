# Module 8 — Type erasure, three ways

Source under study: `entt/core/any.hpp`, `entt/signal/delegate.hpp`,
`entt/poly/poly.hpp`.
Reimplementation: `src/acpp/any.hpp`, `src/acpp/delegate.hpp`.

| Exercise | Deliverable |
|---|---|
| 1 — the comparison table, measured | `erasure_table.cpp`, below |
| 2 — `small_any<Len, Align>`, single-function vtable, `nm` check | `src/acpp/any.hpp`, `small_any.cpp`, `any_emission.cpp` |
| 3 — two-pointer delegate, inlining verified | `src/acpp/delegate.hpp`, `delegate_probe.cpp` |
| 4 — swap a `std::function` callback for a delegate | `delegate_semantics.cpp` |

---

## Checkpoint: "should I use `std::function` here?" — three follow-up questions

1. **Does the callable outlive the call site, and who owns it?** If the wrapper
   must *own* the callable — a lambda with captures, stored past the enclosing
   scope — `std::function` is the right answer and a delegate is a dangling
   pointer waiting to happen. A delegate owns nothing; that is its whole trade.
2. **Is the target known at the binding site?** `delegate.connect<&f>()` binds
   through a template parameter, so the trampoline is a direct call. If the
   target is only known at run time, a delegate degrades to a function pointer
   with a payload and most of its advantage is gone.
3. **What does the memory cost, times how many?** 16 bytes versus 32 does not
   matter for one callback and matters a great deal for a table of a thousand,
   or for a struct that has to fit a cache line. And `std::function` **allocates**
   for a capture larger than its buffer (measured below), which on a no-heap
   target is not a performance question but a correctness one.

Defensible recommendation: `std::function` for ownership and for anything a user
supplies; a delegate for fixed callback tables, driver dispatch and
interrupt-adjacent code; a raw function pointer when there is no instance at all;
a virtual call when the polymorphism is genuinely open-ended and the objects are
already heap-allocated.

## Exercise 1 — the table

`-O2`, gcc 13.3, 5M calls, best-of-7. Every target is chosen at run time (from
`argc`) and every holder is passed through a compiler barrier, because without
that the optimiser devirtualises all of it — the first version of this table
reported a virtual call as **five times faster than a raw function pointer**,
which is not a fact about dispatch.

| mechanism | `sizeof` | allocations | ns/call (best) | (worst) |
|---|---:|---:|---:|---:|
| *baseline: inlined* | — | — | **0.51** | 0.76 |
| raw function pointer | 8 | 0 | 3.62 | 7.71 |
| virtual call | 8 | 0 | 5.24 | 13.93 |
| `acpp::delegate` | 16 | 0 | 6.05 | 10.81 |
| `entt::delegate` | 16 | 0 | 3.57 | 7.08 |
| `std::function` (small) | 32 | 0 | 4.84 | 10.25 |
| `std::function` (heap capture) | 32 | **1** | 6.10 | 16.52 |
| `entt::poly` | 48 | 0 | 5.73 | 7.60 |

**What this table does and does not establish**, which is the honest part:

- **Solid.** `sizeof` and the allocation counts are exact. Inlined dispatch
  (0.51 ns) is *six to ten times* cheaper than any indirect mechanism — that gap
  is far larger than the noise and is the only timing conclusion worth carrying
  away. A `std::function` holding a 32-byte capture allocates exactly once;
  everything else allocates never.
- **Not established.** The ranking *within* the indirect group. This machine is a
  single shared vCPU; the best-to-worst spread on a single row (3–10 ns) is
  larger than the differences between rows. Anyone reporting "delegates are 1.7×
  faster than std::function" from numbers like these is reading noise. The
  `worst` column is printed for exactly that reason.

Getting a defensible ranking would need a machine with pinned cores and no steal
time, and `perf stat` — which `perf_event_paranoid` also denies here (see
`docs/CLAUDE.md`). Flagged rather than faked.

### Measured — 16-core WSL2 (2026-08-27)

`nproc` = 16, i9-9900K, gcc 13.3 `-O2`, Debug build, no steal time, 5 external
repetitions of the whole program (best-per-column across the 5). The machine
label the program printed changed with it — `erasure_table.cpp` hardcoded
"1 vCPU shared cloud instance"; it now prints `hardware_concurrency()` and a
caveat about what more cores do and don't buy a single-threaded benchmark.

| mechanism | `sizeof` | allocations | ns/call (best) | (worst) |
|---|---:|---:|---:|---:|
| *baseline: inlined* | — | — | **0.304** | 0.322 |
| raw function pointer | 8 | 0 | 1.029 | 1.054 |
| virtual call | 8 | 0 | 1.018 | 1.071 |
| `acpp::delegate` | 16 | 0 | 1.258 | 1.280 |
| `entt::delegate` | 16 | 0 | 1.272 | 1.282 |
| `std::function` (small) | 32 | 0 | 1.697 | 1.706 |
| `std::function` (heap capture) | 32 | **1** | 1.483 | 1.491 |
| `entt::poly` | 48 | 0 | 1.637 | 1.705 |

**Yes — the ranking resolves on a quiet machine, and it resolves into three
tiers, not a strict order.** Run-to-run noise on any one row is ≤0.05 ns here
(the droplet's was 3–10 ns), and that noise is now an order of magnitude
smaller than the gaps *between* tiers, so this is a real ranking rather than
a re-reading of the same noise:

1. **raw function pointer ≈ virtual call**, ~1.02–1.06 ns, indistinguishable
   from each other. Both are one indirect call through a pointer-sized
   holder — a vtable slot and a bare function pointer cost the same once
   inlining is off the table.
2. **`acpp::delegate` ≈ `entt::delegate`**, ~1.26–1.28 ns, indistinguishable
   from each other and a fixed ~0.25 ns above tier 1 — the cost of carrying
   an object pointer alongside the function pointer and passing both through
   the call, not a difference between the two implementations.
3. **`std::function` (heap) at ~1.48 ns**, then **`entt::poly`** and
   **`std::function` (small)** essentially tied at ~1.64–1.70 ns, all three
   well above tiers 1–2. `std::function`'s empty-target check (Exercise 3
   below) and `poly`'s extra indirection through a separate vtable object
   both cost more than a delegate's direct object+function pair, and here
   that cost is finally bigger than the noise that hid it on one core.

The single-core table's caution about allocation counts and the inlined-vs-any-
indirection gap both still hold — this only resolves the question that table
explicitly left open, the ranking *within* the indirect group.

## Exercise 3 — the inlining claim, and the folklore it corrected

Codegen, `-O2`, checked by `codegen_delegate_*`:

| probe | body | result |
|---|---|---|
| delegate, local constant, compile-time target | `lea (%rdi,%rdi,2)`, `ret` | fully inlined |
| **`std::function`, local constant** | `lea (%rdi,%rdi,2)`, `ret` | **also fully inlined** |
| delegate, arrives as a parameter | 5 instructions, `jmp *%rax` | one indirect tail call |
| `std::function`, arrives as a parameter | ~16 instructions, `cmpq $0`, `call *24(%rdi)`, a `__throw_bad_function_call` path, stack protector | indirect call **plus an empty-target check** |

The second row is the one that changed a conclusion. The received wisdom is
"delegates inline, `std::function` does not"; at `-O2` gcc devirtualises a *local*
`std::function` with a known target just as completely. That test is written to
**require** it, so if the folklore ever becomes true again the build says so.

The real difference shows up once the callable crosses a function boundary. Then
`std::function` must check for an empty target before dispatching — the standard
requires it to throw `std::bad_function_call` — and that check is not optional,
it is the interface. A delegate has no such check *by design*: it is the
low-overhead option, and adding a branch to every call to catch a programming
error would defeat the point. The contract is "connect before you call", the same
contract a raw function pointer has, and `delegate_semantics.cpp` documents it as
a test rather than as a warning comment.

## Exercise 2 — `small_any`

**The single-function vtable.** One function pointer per `any`, not one per
operation, with every operation a `case` in one `switch`:

```cpp
template<typename Type>
static const void *basic_vtable(operation op, const basic_any &value, const void *other);
```

Three things fall out of it:

- `sizeof(any)` is buffer + one pointer + a policy byte, not buffer + N pointers.
- The compiler sees every operation for a type in one function body, so the ones
  the program never reaches are dead code in a function it already has, rather
  than separate symbols that have to be individually stripped.
- `using enum internal::any_operation;` inside the switch keeps the case labels
  unqualified without leaking the enumerators anywhere — C++20 in the wild.

**The SBO decision** is compile-time, per type, per `<Len, Align>`:

```cpp
(Len != 0) && alignof(Type) <= Align && sizeof(Type) <= Len
          && is_nothrow_move_constructible_v<Type>
```

The nothrow-move requirement is not fussiness. Relocating an embedded object
means moving the buffer, and a throwing move half way through leaves two
half-objects and no way back; a heap-held object relocates by pointer swap, which
cannot throw. So a movable-but-throwing type is pushed to the heap *on purpose*,
and `small_any.cpp` checks that it is.

**`Len == 0` is a deliberate case, not a degenerate one**: it specialises the
buffer away entirely, giving a pointer-only `any` for when everything is heap
allocated anyway.

**Ownership mode as a separate axis.** `any_policy` (`empty`, `dynamic`,
`embedded`, `ref`, `cref`) is orthogonal to the type, so one wrapper holds a
value, a non-owning reference or a const reference instead of three wrapper
types. Two behaviours worth pinning, both tested:

- copying an *alias* copies the alias, not the object;
- a `cref` alias returns `nullptr` from the mutable `data()`, because laundering
  the const away is the one place this design could go silently wrong.

**A size result I did not expect.** `sizeof(shallow_any)` is **24**, not 16: a
pointer, a vtable pointer, and a policy byte that costs a whole word to padding.
libstdc++'s `std::any` is 16 because it has no third field. That one byte costing
eight is precisely the situation Module 11 answers by packing small state into
spare bits of a word that already exists — noted here, paid off there.

**...and the comparison inverts on libc++** (Apple clang 21, arm64, 2026-08-26):

| | libstdc++ (x86-64) | libc++ (arm64) |
|---|---:|---:|
| `sizeof(shallow_any)` | 24 | 24 |
| `sizeof(std::any)` | **16** | **32** |
| `sizeof(acpp::delegate<int(int)>)` | 16 | 16 |
| `sizeof(std::function<int(int)>)` | 32 | 32 |

Ours is unchanged; `std::any` doubles. libc++ gives it a three-pointer inline
buffer plus a handler pointer, where libstdc++ uses one pointer plus a handler.
So the sentence above — "mine is 24 where the standard's is 16" — is a
**libstdc++ fact, not a C++ fact**. On libc++ the same design is 8 bytes
*smaller* than `std::any`, and the "one byte costing eight" complaint buys a
type that beats the standard library rather than losing to it.

Worth keeping both columns for the reason the exercise exists: the SBO capacity
of `std::any` is implementation-defined, so any claim of the form "type erasure
costs N bytes" is a claim about one standard library. `std::function` happens to
agree at 32 on both, which is what makes the `std::any` row easy to miss.

The delegate rows are unchanged, which is the point of them — `acpp::delegate` is
two pointers by construction rather than by a library's SBO policy.

**The `nm` half.** `any_emission.cpp` instantiates `basic_any` for two types and
uses different operations on each; `symbols_any_vtables` runs
`nm -C --size-sort --defined-only` on the object file and requires that no vtable
exists for a type that was never put into an `any`. What `nm` can show with this
design is *which types' vtables were emitted* — because the single-function
vtable deliberately puts all the operations for one type in one symbol. That is
the design working as intended, and it is also the honest limit of the check.

## Exercise 4 — the callback table

`delegate_semantics.cpp` builds the same two-entry callback table twice. Per
entry: **16 bytes** with a delegate against **32** with `std::function`, and the
delegate version is trivially copyable so the vector's reallocation is a `memcpy`
rather than a per-element manager call.

`static_assert(std::is_trivially_copyable_v<delegate>)` and
`static_assert(!std::is_trivially_copyable_v<std::function<int(int)>>)` are the
compact statement of the whole trade.

## A trap outside the module's own code

`entt::poly`'s interface must declare `int(int) **const**`, not `int(int)`, for a
`const` member to call `poly_call`. The const overload of `invoke` hands the
vtable a `const basic_any &`, which only type-checks against a const-qualified
signature. Getting it wrong produces a wall of template errors pointing at
`poly.hpp` rather than at the interface — which is itself the practical argument
in `poly`'s cost column.

## Techniques logged

Added to `docs/notes.md`: SBO with a compile-time fit check, the single-function
vtable, ownership mode as a policy enum, the two-pointer delegate, and
benchmarking against optimiser interference.
