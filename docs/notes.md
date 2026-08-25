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

---

## Module 7 — Storage and view iteration

**Paged payload for pointer stability** · `entt/entity/storage.hpp` ·
`src/acpp/storage.hpp`
Pages are allocated once and never reallocated, so an element's address is stable
for its whole lifetime.
*Use when:* anything outside the container holds a pointer or reference into it.
*Note:* a **different** motivation from the paged *sparse* array, which is about
bounding memory waste from sparse ids. Same mechanism, opposite reasons.
*Caveat:* stable under growth is not stable under erasure — swap-and-pop still
moves the last element. Stability across erasure needs `in_place`.

**Trait-derived policy that also makes the code well-formed** ·
`src/acpp/storage.hpp`
`storage_policy` is derived from `in_place_delete`, and the relocation hooks are
`if constexpr`-guarded on movability, so a non-movable component compiles
*because* the policy that would relocate it was never selected.
*Use when:* an inferred policy is a correctness claim — guard the paths it
excludes, so the compiler enforces the claim instead of a comment asserting it.

**Backward iteration for safe mid-loop removal** · same file
`begin()` at the high end, `++` toward 0, with `index()`, `operator-` and
`operator<=>` all flipped.
*Buys:* erasing the current element under swap-and-pop skips nothing (the
replacement comes from a slot already passed), and elements created mid-loop are
not visited.

**Zip iterators instead of looking up** · same file
A storage iterating itself walks the entity and payload arrays together; only a
*view*, which leads with one pool, has to look entities up in the others.
*Watch for:* the lookup version compiles and gives the right answer, so this is
invisible without a codegen check.

**Smallest-pool selection** · `entt/entity/view.hpp` · `src/acpp/view.hpp`
Scan the candidate pools, lead with the smallest, filter against the rest.
*Measured:* 266× on 1M vs 100 elements.
*Watch for:* a view holds pointers, so "smallest" goes stale — `refresh()` is
explicit, not automatic.

**Compile-time-collapsed filter chain** · same file
`(Get == 1u) || ...` and `(Exclude == 0u) || ...` are constants, so a
single-pool view compiles the whole filter away.

**Iterator composition via `tuple_cat`** · same file
`tuple_cat(make_tuple(*it), pools->get_as_tuple(*it)...)` inside an
immediately-invoked generic lambda over an `index_sequence`.
*Buys:* an empty component contributes an empty tuple and vanishes, with no
special case downstream. Verified free at `-O2`.

**Qualify calls to names you also re-export** · `src/acpp/view.hpp`
An unqualified `all_of(...)` on `std::array` iterators finds `std::all_of` by ADL
and is ambiguous. Qualify to disable ADL.
*Use when:* your library has a `stl::` shim (Module 4) — the hazard is structural.

**Codegen probes need header dependencies** · root `CMakeLists.txt`
An `-S` custom command depending only on its `.cpp` keeps passing against stale
assembly after a header changes. `-MD -MF` plus `DEPFILE` fixes it.
*Use when:* any generated artifact is used as evidence.

---

## Module 8 — Type erasure, three ways

**SBO with a compile-time fit check** · `entt/core/any.hpp` · `src/acpp/any.hpp`
`(Len != 0) && alignof(T) <= Align && sizeof(T) <= Len && is_nothrow_move_constructible_v<T>`.
*The last clause is the one people drop:* relocating an embedded object moves the
buffer, and a throwing move mid-relocation leaves two half-objects. A
throwing-move type belongs on the heap, where relocation is a pointer swap.
*Also:* `Len == 0` specialising the buffer away is a deliberate configuration —
a pointer-only `any` for when everything is heap-allocated anyway.

**Single-function vtable** · same file
One function pointer and one `switch`, instead of one pointer per operation.
*Buys:* smaller objects, and the compiler sees every operation for a type
together. `using enum` keeps the case labels readable.

**Ownership mode as a policy enum** · same file
`empty / dynamic / embedded / ref / cref` is orthogonal to the held type, so one
wrapper covers value, reference and const-reference.
*Watch:* copying an alias must copy the alias; a `cref` alias must refuse mutable
access.

**Two-pointer compile-time-bound delegate** · `entt/signal/delegate.hpp` ·
`src/acpp/delegate.hpp`
An untyped instance pointer plus a fixed-signature function pointer; the target
arrives as a template parameter so the trampoline is a direct call.
*Use when:* fixed callback tables, driver dispatch, interrupt-adjacent code, no
heap. *Not when:* the wrapper must own the callable — a delegate owns nothing.
*Deliberately no null check on call:* a branch per call to catch a programming
error would defeat the point.

**Benchmarking against optimiser interference** ·
`modules/08-type-erasure/erasure_table.cpp`
Choose every target at run time and pass every holder through
`asm volatile("" : : "g"(p) : "memory")` before timing. Otherwise the compiler
devirtualises the lot and the table measures the optimiser.
*And:* report best **and** worst of N. On a shared vCPU the spread was larger
than the differences between mechanisms, which is a result — it says the ranking
is not established. Measured in `modules/08-type-erasure/NOTES.md`.

**Local `std::function` inlines too** · same file
At `-O2` gcc devirtualises a local `std::function` with a known target as
completely as a delegate. The delegate's edge is size, no allocation, trivial
copyability, and the absence of the empty-target check once the callable crosses
a function boundary — not inlining a local.

---

## Module 9 — The Chase–Lev work-stealing deque

**Chase–Lev deque** · `taskflow/core/wsq.hpp` · `src/acpp/wsq.hpp`
One owner pushes and pops at the bottom (LIFO, cache-warm); many thieves steal
from the top (FIFO, coarse-grained, far from the owner's working set). Free-
running `int64` counters, a power-of-two buffer, and a CAS on `top` as the only
place the two sides meet.
*Use when:* per-worker task queues in any scheduler.

**The `seq_cst` fence is for store-load, and nothing weaker will do** · same file
`bottom.store(relaxed); fence(seq_cst); top.load(relaxed);` — the fence stops the
store being reordered after the load. `acq_rel` on a fence orders load-load,
load-store and store-store but **not** store-load, which is the only ordering
needed here.
*Failure mode without it:* owner and thief both see an empty queue and the last
task is lost, or both take it and it runs twice. Interleaving in
`modules/09-work-stealing-deque/NOTES.md`.

**A passing stress test on x86 is not evidence** · same NOTES
The deliberately-weakened build passes 200,000 items with three thieves. x86-TSO
plus a single core cannot produce the window. The written argument is the
evidence; the test only catches regressions the machine can exhibit.

**Relaxed justified by direction, not by speed** · same file
`cached_top` is a monotonic lower bound on a counter that only increases, so a
stale value can only overestimate occupancy — and overestimating costs one extra
load, never a missed resize.
*Use when:* you want to justify a relaxed load. The argument is always "here is
why a stale value is harmless, in the direction it can be stale".

**Cache-line isolation of contended atomics** · same file
`alignas(64)` on `top`, `bottom` and the buffer pointer separately: the owner
writes one, thieves write another, and sharing a line makes every steal attempt
invalidate the owner's.

**Retained-garbage reclamation** · same file
A resized-away buffer cannot be freed — a thief may hold a pointer into it — so
old buffers are retained until destruction.
*Acceptable because:* each resize doubles, so the retained total is bounded by
the final capacity and the count is log₂ of it. Measured: 6 resizes for 200,000
items. The alternative is hazard pointers or epoch reclamation.

**An uncontended mutex is not slow** · `modules/09-work-stealing-deque/NOTES.md`
Single-threaded, the lock-free queue beat `std::mutex` + `std::deque` by only
1.14×. A futex fast path is a couple of atomics. Expect the win from lock-free
under *contention*, not from the uncontended path.

---

## Module 10 — Sleeping without lost wakeups

**Two-phase commit wait** · `taskflow/core/nonblocking_notifier.hpp` ·
`src/acpp/notifier.hpp`
`prepare_wait(id)` → re-check the predicate → exactly one of `cancel_wait(id)` or
`commit_wait(id)`. The announcement is published *before* the re-check, so a
notify arriving after the check still finds somebody to tell.
*Use when:* any worker pool that sleeps. This is the bug every thread-pool author
meets once.
*The instruction that closes the window* is the `fetch_add` in `prepare_wait`
plus the `seq_cst` fence after it — without the fence the announcement can sit in
a store buffer while the predicate load has already retired, which is the same
store-load problem as Module 9's `pop`.
*The rule:* one `prepare_wait`, exactly one resolution. Trace every exit path.

**Packed atomic state word** · same file
`[epoch:32][prewaiters:16][stack:16]` in one `uint64_t`, with the increments
placed so `state - prewaiter_inc + epoch_inc` updates two fields in one CAS.
*Max workers:* 65,534 — 16 stack bits minus the all-ones "empty" sentinel. The
constructor throws rather than aliasing, because the failure is a hang.

**An epoch that is both an ABA defence and a ticket** · same file
Bumped by every CAS that resolves a request, so a stale read cannot win; and
`epoch - recorded` tells a pre-waiter whether its turn has arrived, passed, or
not yet come — serialising resolutions in arrival order without a lock.
*Wraps harmlessly* because every comparison is a signed difference of two nearby
epochs.
*Note the deliberate omission:* popping the waiter stack does **not** bump the
epoch, because a waiter can only be re-pushed after passing through pre-wait,
which always does.

**Reproduce the race, do not wait for it** · `modules/10-notifier/lost_wakeup.cpp`
Block the sleeping thread on a handshake until the pusher has finished, so the
losing interleaving happens every run. Measured: the naive worker sleeps 50.8 ms
with work queued, and only wakes because `wait_for` has a timeout.
*Use when:* any race worth fixing. One-in-a-week is a mystery; one-in-every-run
is a bug.

**Idle CPU: spinning is the outlier, not the notifier** ·
`modules/10-notifier/NOTES.md`
Measured over a bursty load: spin 0.259 s CPU, 2PC 0.0033 s, condvar-per-push
0.0015 s, for identical throughput. Spinning costs ~79×.
*And the honest half:* 2PC did **not** beat condvar-per-push on idle CPU here,
because the harness gives all three the same locked queue. The advantage it is
built for — no synchronisation on push when nobody sleeps — needs a lock-free
push path to show up at all.

**`printf(fmt)` with no arguments is a security warning, not a style one** ·
`src/acpp/testing.hpp`
`-Wformat-security` fires on a non-literal format with no arguments. Split the
zero-argument case into an overload that goes through `"%s"`.

---

## Module 11 — Scheduler and graph representation

**Variant nodes** · `taskflow/core/graph.hpp` · `src/acpp/graph.hpp`
Heterogeneous node kinds in one type: no inheritance, no per-node heap
indirection. Cost is `sizeof` = max over alternatives plus a discriminator.
*Measured:* when every alternative holds a `std::function` they are all the same
size, so the variant costs nothing for heterogeneity — all of it is
type-erased-callable tax.
*And:* the largest member of a task node turned out to be the **edge vector**,
not the variant. Measure before proposing a shrink.

**Partitioned successor/predecessor vector** · same file
One vector split at `num_successors`: push back, swap into the boundary, bump
the boundary. O(1), one allocation, one cache line at typical fan-out.
*Removal* needs two moves, not one, to keep both halves contiguous.

**Small vector with inline capacity** · `src/acpp/small_vector.hpp`
N elements inside the object, heap only beyond.
*Justified by:* the distribution of sizes, not by "vectors are slow". Most task
nodes have ≤4 edges, so most graphs allocate nothing for topology.

**Atomic join counters** · `taskflow/core/graph.hpp`
Each node holds `atomic<size_t>`; on completion a task decrements each
successor's, and whoever drives one to zero owns scheduling it. No locks, no
central scheduler state.
*The reset problem:* a graph that runs twice needs its counters restored — do it
in `run()`, before anything is scheduled, which is the only moment no worker can
be looking.

**Count scheduled nodes, not total nodes** · `src/acpp/executor.hpp`
A completion counter initialised to the node count hangs on any graph containing
a condition, because a condition leaves branches unrun. Raise the counter when a
successor becomes ready, lower it when a node finishes — and raise before the
predecessor's own decrement so it cannot transiently hit zero.

**State split by who touches it** · `src/acpp/graph.hpp`
One plain word for fields only the owning thread reads, one atomic for the rest.
*And pack the refcount into the atomic's spare bits:* not fewer RMWs, but **one
atomic object instead of two** — one cache line bouncing between cores instead of
two, and a word saved per node. Cost is a stated cap on the count.

**Direct continuation via an invoke cache** · `src/acpp/executor.hpp`
A finishing task with exactly one ready successor runs it directly: no push, no
pop, no chance of a steal. Queue traffic only at fan-out.
*Measured:* a 200-link chain runs with 199 continuations and 0 steals.

**Cross-thread exception propagation** · same file
Capture the **first** `exception_ptr`, count the rest, cancel the graph, rethrow
from `wait()` on the calling thread.
*Watch:* cancellation must skip the work, not the bookkeeping, or a cancelled
graph never finishes; `wait()` must rethrow once, not re-arm; and the
`catch(...)` around the invoke is what stops a throw unwinding a worker out of
the pool.

**Sticky victim stealing** · `taskflow/core/executor.hpp`
Remember who you last stole from and try them first.
*Measured:* 23% fewer steal attempts, success rate 14.5% → 19.8% on a
pipeline-shaped graph.

**Sampling from a set minus one element** · same file
`v = rand() % (N-1); if(v >= self) ++v;` — one modulo, one predicated increment,
no rejection loop and therefore no unbounded worst case. Total for `N >= 2`.

**`std::visit` is not obviously worse than switch-on-index** ·
`modules/11-scheduler/NOTES.md`
Measured, gcc 13.3 `-O2`, 4 alternatives: visit 22 instructions, switch 27,
if-else 27. `visit` is exhaustive by construction, so it emits no `default` case.
The real reasons to prefer the switch are structural (a dedicated function per
kind), not codegen.

---

## Module 12 — Parallel algorithm design

**Contract enum vs strategy class** · `taskflow/algorithm/partitioner.hpp` ·
`src/acpp/partitioner.hpp`
Four partitioner classes, two enum values: the enum encodes the *scheduling
contract* the algorithm must branch on (pre-assigned ranges vs a shared cursor);
the class encodes the *chunk-sizing strategy*, which never leaves `loop()`.
*Use when:* you have two customization axes and only one of them belongs in the
skeleton. Adding a strategy then costs nothing; adding a contract costs a rewrite.

**Partitioner as a defaulted template policy** · same file
The common case costs nothing at the call site and the specialist case needs no
library change. `sizeof(dynamic_partitioner<>) == sizeof(size_t)`.

**A tag type, not `void`, for "no policy"** · same file
`Closure = void` makes a member taking `Closure` ill-formed at *class*
instantiation, and a `requires` clause cannot rescue it. An empty tag type with a
call operator costs nothing under `[[no_unique_address]]`.

**Guided needs a CAS where dynamic needs a `fetch_add`** · same file
The size of a guided claim depends on what is left, so it must be computed from a
value and committed against that same value. That is the structural difference,
and it is why guided pays more per chunk and wins only when the tail is uneven.

**Closure wrapper injection point** · same file
The user wraps every chunk in their own setup/teardown — a thread-local scratch,
NUMA pinning, a profiling scope — and the algorithm never learns about it.
*Caveat:* the wrapper must be at namespace scope. A local class cannot have
member templates; gcc allows it as an extension, clang does not.

**Cooperative blocking (`corun`)** · `taskflow/core/runtime.hpp` ·
`src/acpp/executor.hpp`
A worker that must wait for nested work re-enters the scheduling loop with a
completion predicate instead of blocking, so it spends the wait executing the
work it is waiting for.
*Without it:* W outer tasks each blocking on nested work deadlock a W-worker
pool. Reproduced in `modules/12-parallel-algorithms/corun_deadlock.cpp`.
*Two properties that make it a fix:* it must never park, and it must nest.
*And:* join implicitly, in the invoke path — forgetting to join is the whole
failure mode, so it should not be something a user can forget.

**Lock-free serial pipeline stages** · `taskflow/algorithm/pipeline.hpp` ·
`src/acpp/pipeline.hpp`
A per-stage atomic ticket: a serial stage admits token `t` only when its counter
reads `t`, and publishes `t+1` on the way out. A release/acquire handoff between
two tokens, not mutual exclusion.
*The invariant that makes it terminate:* every claimed token walks every stage
and advances every ticket, even after the stream has stopped. Returning early
leaves later stages waiting for a value nobody will publish.
*And:* a line waiting for its ticket coruns rather than blocking.

---

## Capstone — reactive dataflow

**Partial topological execution** · `src/acpp/dataflow.hpp`
Dirty only the transitive dependents of what changed; recompute only those, with
edges only between cells that are *both* dirty.
*That last clause is the whole design:* an edge to a clean cell drags it, and its
inputs, back into the recomputation.
*Measured:* 15.3× over full recomputation when 27% of the graph is affected,
1.23× when 72% is. The benefit is the reciprocal of the affected fraction.

**A sparse set is the right dirty set** · same file
Needs O(1) membership (for the marking early-out and the edge filter) *and* dense
iteration (to build one task per dirty cell). A `std::set` gives the second
badly; a `bool` per cell cannot give the second at all.

**Benchmarks that quietly measure nothing** ·
`modules/13-capstone/NOTES.md`
Two versions of the dataflow benchmark produced plausible numbers while measuring
nothing: "best of N" was won by passes where the update was a no-op, and some
graph inputs had no dependents at all. A benchmark that crashes gets fixed; one
that quietly measures nothing gets quoted.
*Habit:* assert inside the measurement that the work actually happened.

**Report the legs that isolate each effect** · same file
`incremental-serial` vs `full-serial` isolates work avoided; `incremental-parallel`
vs `incremental-serial` isolates parallel execution. Two legs instead of three
would have shown a 15× win at one size and a loss at another, and explained
neither.

---

## Phase C — what TSan found that the functional tests did not

The executor passed all 61 tests and had four real defects. Recording them
together because they are a *class*, not four unrelated slips.

**Destroying a condition variable while a notify is in flight** ·
`src/acpp/executor.hpp`
The waiter owns the topology, so it can destroy the `condition_variable` while
the notifier is still inside `notify_all`.
*Fix:* notify **inside** the lock — the opposite of the usual advice, which
assumes the condition variable outlives both parties.

**Touching an object after releasing the thread that owns it** · same file
`finish_node` can complete a run, releasing `wait()`, letting the waiter destroy
the graph. Any read of the node afterwards is a use-after-free.
*Rule:* read everything you need from an object **before** the operation that
can let someone else destroy it.

**A "harmless" statistics race** · same file
Plain counters written by a worker and read by the caller.
*Fix:* relaxed atomics, not synchronisation. That removes the race without
adding ordering, and "may be slightly stale" is the correct contract for a
counter nobody schedules on.

**Identity captured at construction, used from another thread** · same file
`runtime` recorded the worker that created it; its spawned work runs anywhere.
Two threads then drove one worker's deque and RNG.
*Rule:* for "which worker am I", the answer is `thread_local`, never a field
captured earlier.

**A lost wakeup inside the lost-wakeup fix** · `src/acpp/notifier.hpp`
`park()` wrote `state = waiting` on entry, erasing a signal that arrived between
the CAS publishing the waiter and this thread taking its mutex. The 2PC closes
the window between "I checked" and "I registered"; this reopened one between "I
registered" and "I slept".
*Rule:* state a waiter publishes must not be re-initialised after it becomes
reachable — clear it before publishing, and let the park only read.
*And:* **TSan cannot see this.** Every access is under the mutex, so there is no
data race; it is a protocol error. TSan-clean is necessary and nowhere near
sufficient.

**And one only a four-core CI runner found:** the pipeline's stop point was a
`bool`, so a token still queued for a later stage when the source stopped would
skip that stage and be dropped. Fixed by making the stop point a *token*
(`token >= stop_token`). Passed 25 local runs and TSan; failed CI's first run.
*The lesson:* a single-core machine cannot validate a concurrent design. It
finds protocol errors, TSan finds races, and neither finds this.

**And one a stopwatch found:** the corun wait was an unbounded yield-spin, which
is fine at one waiter per core and pathological above it — the pipeline test did
not finish in 500 s under TSan. A **bounded** backoff (yield, then sleep 50 µs)
fixes it without becoming the indefinite park that corun exists to avoid.
