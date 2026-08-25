# Module 12 — Parallel algorithm design

Source under study: `taskflow/algorithm/partitioner.hpp`, `for_each.hpp`,
`reduce.hpp`, `pipeline.hpp`, `taskflow/core/runtime.hpp`.
Reimplementation: `src/acpp/partitioner.hpp`, `src/acpp/algorithm.hpp`,
`src/acpp/pipeline.hpp`, plus `runtime::corun` in `src/acpp/executor.hpp`.

| Exercise | Deliverable |
|---|---|
| 1 — static / dynamic / guided behind one interface, benchmarked | `src/acpp/partitioner.hpp`, `partitioner_bench.cpp` |
| 2 — reproduce the nested-blocking deadlock, then fix it | `corun_deadlock.cpp` |
| 3 — a three-stage pipeline, output ordering verified | `src/acpp/pipeline.hpp`, `pipeline_ordering.cpp` |
| 4 — `closure_wrapper` for a per-worker scratch buffer | `pipeline_ordering.cpp` |

---

## The structural point: four classes, two enum values

There are **four** partitioner classes and the type enum has **two** values.
Guided and random both report `is_dynamic`. That asymmetry is the design lesson
of the file and it is easy to read past.

- The **enum encodes the scheduling contract** — the only thing the algorithm
  skeleton needs to branch on. `is_static` means ranges are pre-assigned per
  worker with no shared state; `is_dynamic` means workers pull chunks from a
  shared cursor. `algorithm.hpp` branches on exactly this and nothing else.
- The **class encodes the chunk-sizing strategy** — uniform, guided shrink,
  random. It lives entirely inside `loop()` and never leaks into the algorithm.

Two customization axes, deliberately kept at different visibility levels. Adding
a fifth strategy requires no change to `for_each_index` or `reduce_index`;
adding a third *contract* would.

**The partitioner is a template parameter with a default**, so the common case
costs nothing at the call site and the specialist case needs no library change.
`sizeof(dynamic_partitioner<>) == sizeof(size_t)` — the default `no_closure` is
empty and `[[no_unique_address]]` gives it no storage, which is Module 3's
technique landing exactly where it is needed.

### A C++ detail worth recording

The closure parameter defaults to a **tag type**, `no_closure`, not to `void`.
`Closure = void` makes `partitioner_base(size_t, Closure)` ill-formed at *class*
instantiation — a parameter of type `void` — and a `requires` clause on the
constructor cannot rescue it, because the declaration is checked regardless.
An empty tag type with a call operator solves it and costs nothing.

## Exercise 1 — the three strategies

**Static** — even split, no shared state. `chunk_size == 0` means "auto":
`N/W + (w < N%W)`, so the remainder is handed out one item at a time to the first
`N % W` workers and no worker is more than one item ahead of another. Each worker
then **strides** by `W * chunk` rather than taking one contiguous block, which is
what stops a static split from being catastrophic when cost varies across the
index range.

**Dynamic** — uniform chunks from a shared `fetch_add` cursor. Perfect load
balance, one atomic RMW per chunk. The chunk size is the whole tuning knob:
too small and the cursor becomes the bottleneck, too large and the tail
imbalances.

**Guided** — large chunks first, shrinking toward the end:
`take = max(chunk, 0.5/W * remaining)`. Large chunks amortise the cursor;
small chunks at the end give fine-grained balancing on the tail, where one slow
item would otherwise make everyone wait. Below `2*W*(chunk+1)` remaining it
switches to plain `fetch_add` chunks, because under that threshold a CAS loop
costs more than it saves.

It has to be a **CAS**, not a `fetch_add`: the size of the claim depends on what
is left, so it must be computed from a value and then committed against that same
value. That is the structural difference between guided and dynamic, and it is
why guided pays more per chunk.

**Random** — chunk sizes drawn from a range. Not a joke: when item cost is
adversarial to a fixed chunking (every 8th item expensive, chunk size 8),
randomising the boundaries stops the pathology lining up with the schedule.

### Measured

4000 items, 4 workers, gcc `-O2`, best of 3, milliseconds:

| workload | static | dynamic | guided | random |
|---|---:|---:|---:|---:|
| uniform | 23.98 | 24.00 | 27.87 | **23.23** |
| cost proportional to index | 24.82 | 24.61 | **23.92** | 31.72 |
| heavy tail | 22.69 | **20.82** | 20.83 | 27.44 |

**The exercise predicts each partitioner wins one workload. That is not what
happened, and the reason is the machine.** `nproc` is 1, so there is no
parallelism for a partitioner to balance: the total work is identical in every
cell of the table, and load imbalance costs nothing when there is only one core
to leave idle. What the table actually shows is **scheduling overhead**, and by
that measure the spread is ~10% and mostly noise.

Two things do survive:

- **guided is the most expensive on uniform work** (27.87 vs 23.23). Its CAS
  loop is pure overhead against a workload that needed no coordination at all,
  and it is the one result here that the machine cannot explain away.
- **random is the worst on proportional cost** (31.72), which is the shape it is
  least suited to: the cost gradient is smooth and predictable, so randomising
  chunk boundaries adds variance for nothing.

The prediction is not disproved; it is **untested**, and saying so is the only
honest reading. It needs a multi-core machine.

The chunk-sizing behaviour *is* exact and machine-independent, and the benchmark
prints it:

```
static, N=4000 W=4, auto chunk per worker: 1000 1000 1000 1000
static, N=10   W=4, auto chunk per worker:    3    3    2    2   <- remainder one at a time
```

### Predicting load imbalance from the wrong partitioner (the checkpoint)

Given a workload shape, the imbalance a static split produces is
`max_worker_cost - mean_worker_cost`:

| workload | static split | why |
|---|---|---|
| uniform | ~0 | every stride carries the same cost |
| cost proportional to index | **small, because of striding** | interleaved chunks sample the whole range; a *contiguous* static split would give the last worker ~3× the first |
| heavy tail (2% of items 300× cost) | **large and unpredictable** | imbalance is determined by where the expensive items land, which no static rule can know |

Guided is the right answer for the third and the wrong answer for the first,
where its CAS loop is pure overhead over a split that needed no coordination at
all.

The practical version of this: on a **big.LITTLE** layout, or a core carrying
more interrupt load than its peers, the *workers* are heterogeneous even when the
items are uniform — and that makes static exactly wrong and guided exactly right
regardless of the workload's shape.

## Exercise 2 — `corun`, and the deadlock it prevents

If a task calls `wait()` on nested work from inside a worker, that worker blocks.
Do it on every worker at once and the pool has no threads left to run the nested
work, so the wait never ends. `corun_deadlock.cpp` reproduces exactly that on a
separate thread with a deadline, because a test that demonstrates a deadlock has
to survive it.

`runtime::corun()` re-enters the scheduling loop with a completion predicate, so
the blocked worker spends the wait **executing the work it is waiting for**. Two
properties that make it a fix rather than a mitigation, both tested:

- **It must not park.** Sleeping inside corun is the deadlock it exists to avoid.
  The loop yields when there is nothing to help with; it never calls
  `commit_wait`.
- **It must nest.** corun is called from inside work that corun is already
  running. If it only worked one level deep it would not be a fix.

A runtime task joins its spawned work **implicitly**, in `invoke`, rather than
requiring the user to call `corun()`. Forgetting to join is the entire failure
mode, so it should not be something a user can forget.

This is the same problem TBB solves with `task_arena` and that Grand Central
Dispatch is famous for not solving well.

## Exercise 3 — the pipeline

Stages are `serial` (tokens in order, one at a time) or `parallel` (any number,
any order). The interesting constraint is the serial one, and the interesting
question is how to enforce it **without a lock**.

The answer, as in Taskflow: a **per-stage atomic ticket**. A serial stage admits
token `t` only when its counter reads `t`, and publishes `t + 1` on the way out.
That is a release/acquire handoff between two tokens, not mutual exclusion —
there is never a moment where one thread holds something another must wait to
acquire, so there is nothing to be blocked on and nothing to invert priority.

A line waiting for its ticket calls `corun_until`, so the worker executes other
lines' work while it waits. Blocking there would deadlock the pool at exactly the
width where a pipeline gets interesting.

### The invariant that makes it terminate

**Every claimed token walks every stage and advances every serial ticket, even
when there is no longer any work to do.** The first version returned early once
the stream stopped, and left the stages after that point waiting forever for a
ticket value nobody would publish. `stop()` therefore marks the stream stopped
and the token continues through the remaining stages doing nothing but advancing
tickets.

Checked in `pipeline_ordering.cpp`: 200 tokens through serial → parallel →
serial, output leaving the final stage in **strict token order** while the middle
stage ran them concurrently; plus the all-serial degenerate case, which catches
an ordering bug that a parallel middle stage would hide.

Memory is bounded by the number of **lines**, not by the length of the stream —
`buffer` is sized by lines. That is the property that makes a pipeline a
pipeline rather than a fancy `for_each`.

## Exercise 4 — the closure wrapper

The injection point: a user wraps every chunk in their own setup and teardown,
and the algorithm never learns about it. In `pipeline_ordering.cpp` the wrapper
hands each chunk a `thread_local` scratch buffer allocated once per worker,
against a body that allocated its own per item.

Because the wrapper is a template parameter, the no-wrapper case has no storage
and no indirection — `wrap()` is a direct call.

*(Detail that cost a compile: the wrapper must be at namespace scope. A local
class cannot have member templates; gcc accepts it as an extension, clang
correctly refuses.)*
