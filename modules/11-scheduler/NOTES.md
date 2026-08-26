# Module 11 — Scheduler and graph representation

Source under study: `taskflow/core/graph.hpp`, `Executor::_invoke` / `_schedule`
in `core/executor.hpp`, `core/topology.hpp`.
Reimplementation: `src/acpp/graph.hpp`, `src/acpp/executor.hpp`,
`src/acpp/small_vector.hpp`.

| Exercise | Deliverable |
|---|---|
| 1 — `sizeof(Node)`, what drives it, a shrink and its cost | `node_layout.cpp`, below |
| 2 — the partitioned edge vector, property-tested | `edge_partition.cpp` |
| 3 — a minimal DAG executor on Modules 9 + 10 | `src/acpp/executor.hpp`, `dag_executor.cpp` |
| 4 — exception propagation, with the multi-throw decision | `exception_propagation.cpp` |
| 5 — a refcount packed into spare atomic bits | `node_layout.cpp` |
| §11.1 — switch on `index()` vs `std::visit` | `variant_dispatch_codegen.cpp`, below |
| Module 10's exercise 6 — sticky victim | `sticky_victim_bench.cpp`, below |

---

## Checkpoint: one task node's lifecycle, with every atomic named

```
construction        node built in place in a unique_ptr; edges are raw pointers,
                    so nodes must never move -- hence unique_ptr per node rather
                    than vector<node>.                        [no atomics]

precede(other)      edges.push_back, swap into the boundary, ++successor_count;
                    other.edges.push_back(this).              [no atomics -- graph
                                                               construction is
                                                               single-threaded]

run()               join_counter.store(num_predecessors, relaxed)   [1 store]
                    estate_word.store(none, relaxed)                [1 store]
                    topology.pending.store(sources.size(), relaxed) [1 store]
                    relaxed throughout: nothing is running yet, and the
                    schedule() below carries the release.

schedule()          queue.try_push -> bottom.store(release)         [Module 9]
                    notifier.notify_one() -> fence(seq_cst),
                                             state.load(acquire),
                                             state.CAS(acquire)     [Module 10]

pick up             queue.pop()  -> bottom.store(relaxed),
                                    fence(seq_cst),
                                    top.load(relaxed),
                                    maybe top.CAS(seq_cst)          [Module 9]
                 or queue.steal() from another worker

invoke              topology.cancel.load(acquire)                   [1 load]
                    the work runs
                    on throw: estate_word.fetch_or(release)         [1 RMW]
                              topology.exceptions.fetch_add(acq_rel)[1 RMW]
                              topology.cancel.store(release)        [1 store]

release successors  per successor: join_counter.fetch_sub(acq_rel)  [1 RMW each]
                    whoever drives one to zero owns it;
                    topology.pending.fetch_add(acq_rel)             [1 RMW each]
                    the first ready successor is kept as a continuation --
                    no queue, no notify.

finish              topology.pending.fetch_sub(acq_rel)             [1 RMW]
                    if it hits zero: mutex, finished = true, cv.notify_all
```

Steady-state cost of one task with one successor: **two RMWs** (the successor's
join counter, the topology's pending count) and no queue traffic at all, because
the continuation cache keeps it on this worker. Measured: a 200-link chain runs
with **199 continuations and 0 steals**.

## Exercise 1 — `sizeof(node)`, and the thing I got wrong

Measured, x86-64, gcc 13.3, libstdc++:

| member | bytes |
|---|---:|
| `small_vector<node *, 4>` | **56** |
| `std::variant<static, condition, runtime, async>` | 40 |
| `std::string label` | 32 |
| `atomic<size_t> join_counter` | 8 |
| `size_t successor_count` | 8 |
| `nstate` + `atomic<estate>` | 8 |
| `topology *` | 8 |
| **total** | **160** |

`sizeof(tf::Node)` is **216** for comparison — the real one has more node kinds,
subflows and semaphores.

### Re-measured on libc++ — Apple clang 21, arm64 (2026-08-26)

`sizeof(node)` is **160** here too, and that agreement is a coincidence worth
unpicking rather than a confirmation. Full layout from
`clang -Xclang -fdump-record-layouts`, which lists every member rather than the
ones I chose to tabulate:

| offset | member | bytes |
|---:|---|---:|
| 0 | `std::variant<...> handle` | 40 |
| 40 | `small_vector<node *, 4>` | 56 |
| 96 | `stl::size_t successor_count` | 8 |
| 104 | `atomic<size_t> join_counter` | 8 |
| 112 | `nstate::type nstate_word` | 4 |
| 116 | `atomic<estate::type> estate_word` | 4 |
| 120 | `topology *run` | 8 |
| 128 | `atomic<size_t> *async_counter` | 8 |
| 136 | `std::string label` | 24 |
| | **total** | **160** |

`std::string` is **24** on libc++ against **32** on libstdc++ — the one member
that moved. And `sizeof(tf::Node)` is **208** here, not 216, for the same reason:
Taskflow's node also holds a `std::string`.

**Two problems with the table above this one, both found by the second standard
library.** It lists seven members; the class has nine. `async_counter` is missing
(it landed in the same commit, so this is an omission, not drift), and so the
seven rows adding to exactly 160 is arithmetic that happens to land on the right
answer. With `std::string` at 32 and `async_counter` at 8, the libstdc++ members
sum to **168**, not 160.

So either the recorded 160 or the recorded per-member breakdown is wrong for
libstdc++, and I cannot tell which from here — `sizeof(node)` is `suite.note`d,
never `static_assert`ed, so both values pass the build and CI never had an
opinion. **Flagged for the x86 box**: re-run `node_layout` under gcc/libstdc++
and dump the record layout the same way. Whichever number is wrong, the fix is
the same one Module 3 and Module 7 already apply — assert the layout in code so
prose cannot drift from it. A `suite.note` is not a measurement anyone can
regress.

The exercise's conclusion survives on both Unix standard libraries: the **edge
vector is the largest member** at 56 bytes, ahead of the variant's 40, on
libstdc++ and libc++ alike.

**MSVC reverses it**, which the 2026-08-26 CI run found by failing the
`static_assert` that pinned the comparison. The ordering rests entirely on
`sizeof(std::function<void()>)` — 32 bytes on libstdc++ and libc++, 64 on MSVC.
At 64 the variant becomes 72 and overtakes the 56-byte edge vector.

So "what should I shrink first?" has two different correct answers depending on
the standard library, from identical source. The comparison is therefore reported
at run time now, and what `node_layout.cpp` asserts instead are the two
structural facts that hold everywhere: the variant costs its largest alternative
plus a discriminator, and the edge vector inlines four pointers before it
allocates. Both survive a 64-byte `std::function`; the ranking does not.

The wider lesson is the one the sanitizer legs keep teaching in a different key.
A layout claim is only as portable as the implementation detail underneath it,
and `sizeof(std::function)` is exactly such a detail.

**The prediction I wrote before measuring was wrong**, and it is the useful part.
I expected the variant to dominate, because that is what the course's framing
points at ("`sizeof(Node)` is the max over all alternatives plus a
discriminator"). On libstdc++ it does not: the **edge vector is the largest
member**, and a task node spends more of itself on its topology than on its work.
The irony is that the course's framing is right on MSVC and wrong here — I got
the correct answer for the wrong standard library, which is not the same as being
right.

Every variant alternative holds a `std::function`, so they are all the same size
— which means the variant costs *nothing* for heterogeneity. All 40 bytes are
type-erased-callable tax, and the discriminator is free in the padding.

### The shrink proposal, and what each part costs elsewhere

1. **`small_vector`'s `size`/`capacity` as `uint32_t`** (−8), and drop the
   `store` pointer for a one-bit "inlined" flag (−8). LLVM's `SmallVector` does
   the first. *Cost:* a cap on edges per node (4 billion, fine) and an extra
   branch in `data()`.
2. **`std::string label` → `const char *` into an arena the taskflow owns**
   (−24). *Cost:* names must outlive the graph, so a name built at run time needs
   an arena allocation instead of being self-contained.
3. **`std::function` → Module 8's delegate plus a separately-owned closure**
   (−24 on the variant). *Cost:* a delegate does not own its callable, so every
   task's closure needs a home with the right lifetime. That is a design change,
   not a swap — and it is precisely why the real library ships `std::function`.

160 → roughly 104 bytes. Whether it is worth it depends on graph size: at a
thousand nodes it saves 56 KB and changes nothing; at ten million it is the
difference between fitting in cache and not.

## Exercise 2 — the partitioned edge vector

One vector, split at `successor_count`. Adding a successor:

```cpp
edges.push_back(v);
std::swap(edges[successor_count++], edges[edges.size() - 1]);
v->edges.push_back(this);      // lands in v's predecessor half by construction
```

O(1), one container, at most one allocation, and one cache line for the typical
fan-out of four. Removal is the inverse and needs two moves rather than one — the
last successor fills the hole, then the first predecessor fills the vacated
boundary slot — so both halves stay contiguous without shifting anything.

`edge_partition.cpp` property-tests it: 3000 randomized add/remove operations
over 12 nodes against a `std::map<node*, std::set<node*>>` shadow model,
checking after every step that both halves match *and* that the partition
accounts for every edge exactly once. The second check is what catches an
overlap that the first would miss.

## Exercise 3 — the executor

The integration point. Per-worker `bounded_wsq<node *, 8>` from Module 9, a
`nonblocking_notifier` from Module 10, and a locked overflow queue for
submissions from outside the pool and for work that does not fit.

**The continuation cache** is the piece worth building for its own sake. When a
finishing task has exactly one ready successor, the worker runs it directly:

```cpp
begin_invoke:
    ... run target ...
    cache = release_successors(target, chosen);   // keeps the FIRST ready one
    if(cache) { target = cache; goto begin_invoke; }
```

No push, no pop, no window in which another worker can steal a task that is
already hot in this core's cache. Queue traffic happens only at fan-out — which
is where parallelism actually comes from, so it is exactly where you want it.
The `goto` is the honest spelling; nested loops here are worse.

### The bug that shape produced

`topology::pending` was initialised to the graph's node count, and `wait()`
blocked until it reached zero. That works for every graph without a condition in
it — and hangs on every graph with one, because a condition takes one branch and
leaves the others unrun, so "every node finished" is never true.

The fix is that **`pending` counts scheduled nodes, not total nodes**: raised
when a successor becomes ready, lowered when a node finishes. The raise happens
before the predecessor's own decrement, so the counter cannot transiently reach
zero mid-graph.

Found by the conditions test hanging, which is the argument for writing the
degenerate cases (empty graph, single node, cycle, conditions) as tests rather
than assuming the happy path generalises. A graph with no source is now reported
as cancelled rather than hanging, for the same reason.

## What TSan found that 61 passing tests did not

Phase C's ground rule is that nothing concurrent is believed until it is
TSan-clean. The executor passed every functional test and had **three real
bugs**, each of which is a use-after-free or a data race that would surface as
"it crashes about once a day in production".

**1. Destroying a condition variable while a notify is in flight.**

```cpp
{ std::lock_guard g{run.mutex}; run.finished = true; }
run.cv.notify_all();                    // <-- waiter may already be gone
```

`wait()` returns as soon as the predicate holds, and the waiter owns the
topology — so it can destroy the `condition_variable` while this thread is still
inside `notify_all`.

The fix is to notify **inside** the lock, which is the opposite of the usual
advice. That advice assumes the condition variable outlives both parties; here
the waiter owns it. Holding the lock across the notify means the waiter cannot
leave `wait()` — it must reacquire the mutex — until `notify_all` has returned.

**2. Reading a node after the run that owns it has completed.**

```cpp
if(run != nullptr) finish_node(*run);       // may release wait() on another thread
if(target->async_counter != nullptr) ...    // <-- graph may already be destroyed
```

`finish_node` can drive the topology's counter to zero, which releases `wait()`,
which lets the waiting thread destroy the `taskflow` and every node in it. Any
read of `target` afterwards is a use-after-free. Everything needed from the node
is now hoisted above the `finish_node` call.

**3. Statistics counters racing with `reset_stats`.**

Plain `size_t` counters written by a worker in the steal loop and read or zeroed
by the caller. A genuine data race, on data nobody schedules on.

Fixed by making them **relaxed atomics** rather than by adding synchronisation.
That removes the *race* without adding any ordering: a statistic may be read
slightly stale, and that is the correct contract for a counter that exists only
to be printed. Single writer per counter, so a load/store pair suffices — no RMW
in the hot path.

None of these is exotic, and none of them is the kind of thing the functional
tests could have caught, because all three depend on which thread wins a race
that usually has an obvious winner.

## Exercise 4 — exceptions across worker threads

**The decision: the first captured exception is rethrown from `wait()`; the rest
are counted and discarded.** Stated at the top of `exception_propagation.cpp`,
because the course is right that this has no universally correct answer and the
deliverable is a documented choice.

Why not the alternatives:

- *Throw them all* is not expressible. There is no `std::exception_ptr` meaning
  "these three", and `std::nested_exception` chains a cause, not siblings.
- *Throw the last* is arbitrary in a way "first" is not: the first is the one
  whose failure most plausibly caused the others, because the graph is cancelled
  the instant it lands.
- *Collect into a vector and throw that* moves the problem to the caller, who now
  has to handle a type they did not throw. Right for a library that owns its
  error type; wrong for one propagating the user's.

`exception_count()` exposes the rest, so a caller can distinguish "one task
failed" from "the graph collapsed". Measured: of 8 simultaneous throwers,
typically **1** reports before cancellation takes effect — which is why the test
reports the number rather than asserting a fixed one.

Three properties that are easy to get wrong and are therefore tests:

- **`wait()` rethrows once**, then reports success. Re-arming would make a second
  wait look like a second failure.
- **A cancelled graph still drains.** Cancellation skips the *work*, not the
  bookkeeping — a cancelled node still decrements `pending`. Skipping both is how
  you get a cancelled graph that never finishes.
- **The pool survives.** The `try`/`catch(...)` around the invoke is what stops a
  throw unwinding a worker out of the pool.

## Exercise 5 — the packed refcount

The reference count lives in the low 24 bits of `estate_word`, with the flags
above it:

```
 31      26 25         24 23                                   0
┌──────────┬─────────────┬───────────────────────────────────────┐
│ reserved │ cancelled   │            refcount (24 bits)         │
│          │ exception   │                                       │
└──────────┴─────────────┴───────────────────────────────────────┘
```

**What the measurement actually says**, which is not quite what the exercise
implies: the RMW *count* is unchanged — one increment, one decrement either way.
What changes is **how many distinct atomic objects those RMWs touch**: one
instead of two, so one cache line instead of two bouncing between cores, and
eight fewer bytes per node. That is a structural result, not a timed one, and
reporting it as a timing improvement would have been the wrong claim.

The cost is a stated limit: 24 bits caps the count at 16,777,215. Asserted, so it
is a documented constraint rather than a latent overflow.

## §11.1 — switch on `index()` versus `std::visit`

The course is explicit that the visit comparison is *inference*, not
source-attested: Taskflow's comment says "switch is faster than nested if-else
due to jump table", which is a claim about if-else chains. So both were compiled
and the assembly compared, gcc 13.3, `-O2`, four alternatives.

| dispatch | instructions in the body |
|---|---:|
| `switch` on `index()` + `get_if` | 27 |
| `std::visit` | **22** |
| if-else chain of `get_if` | 27 |

**`std::visit` produced the *smallest* code**, and the reason is worth more than
the number: `std::visit` is **exhaustive by construction**, so there is no
`default` case to emit. The switch needs `default: return 0;` for an index the
compiler cannot rule out, and that costs a comparison and a branch. The visit
version also hoisted the common `movl (%rdi), %eax` above the branch.

Neither uses an indirect call or a jump table — with four alternatives gcc chose
a compare chain for both. And the switch and the if-else chain compiled to
*identical* instruction counts, which means the source comment's stated
justification does not reproduce here either.

Conclusions: the reasons to prefer the switch are the ones the source actually
gives — each case can call a dedicated `_invoke_*_task` function, keeping
per-kind logic in separately optimisable bodies — and **not** codegen. On this
compiler, codegen mildly favours `visit`. Which is exactly why the course says
to stop guessing and build both.

## Module 10, exercise 6 — sticky victim, measured

After a successful steal, try the same victim first next time. Pipeline-shaped
graph, 20 stages × 32 tasks, 4 workers, best of 5 runs:

| | steal attempts | succeeded | success rate | sticky hits |
|---|---:|---:|---:|---:|
| random victim | 2016 | 293 | 14.5% | — |
| sticky victim | **1547** | 306 | **19.8%** | 274 |

**23% fewer steal attempts and a success rate up by a third.** The mechanism is
exactly as advertised: producer/consumer relationships in a task graph are
stable, so the worker that had work a moment ago usually still does.

*Caveat, and it is a real one:* this box has one core, so the workers time-slice
rather than run in parallel and the numbers reflect the OS scheduler's
interleaving as much as the heuristic. The first version of this benchmark
measured **zero steals** because each task was too short for a worker ever to be
preempted mid-stage; the per-task work had to be raised to 60,000 iterations
before stealing happened at all. Read the ratio, not the absolute numbers, and
re-run on a multi-core machine before quoting it anywhere.

**Victim sampling without a rejection loop**, worth stealing on its own:

```cpp
victim = random() % (workers - 1);
if(victim >= self.id) ++victim;
```

A draw over `[0, N-1)` mapped onto `[0, N) \ {self}` with one modulo and one
predicated increment. Rejection sampling has unbounded worst-case latency; this
has none. Total only for `N >= 2`, which the constructor guarantees. (`%` over a
non-power-of-two range has modulo bias, so "uniform" is approximate — negligible
here, and being waved away knowingly.)

## Techniques logged

Added to `docs/notes.md`: variant nodes, the partitioned edge vector, atomic join
counters, state split by ownership, the packed refcount, the continuation cache,
cross-thread exception propagation, and sticky-victim stealing.
