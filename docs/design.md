# Capstone design — a reactive dataflow engine

Option B from the course's capstone list: values as cells, dependencies as a
DAG, recomputation as a **partial** topological execution, with change
propagation that dirties only the transitive dependents of what was touched.

Implementation: `src/acpp/dataflow.hpp`.
Correctness: `modules/13-capstone/dataflow_engine.cpp`.
Numbers: `modules/13-capstone/dataflow_bench.cpp`.

Every non-obvious choice below is stated with the alternative it beat, because a
design document that only lists what was built is a description, not a design.

---

## What it is made of

| Piece | From | Doing what |
|---|---|---|
| `cell_id` | Module 5 | a generational handle; staleness is detectable, not preventable |
| `basic_sparse_set<cell_id>` | Module 6 | the dirty set: O(1) membership *and* dense iteration |
| `bounded_wsq<node *>` | Module 9 | per-worker task queues inside the executor |
| `nonblocking_notifier` | Module 10 | workers sleep between recomputes without lost wakeups |
| `taskflow` + `executor` | Module 11 | the partial topological execution itself |

The point of the capstone is that these are not five independent pieces bolted
together — the dirty set's `contains` is what makes dependency marking linear,
and the executor's join counters are what make the recomputation parallel
without a scheduler of its own.

## Decision 1 — the dirty set is a sparse set, not a `std::set` or a flag per cell

**Chosen:** `basic_sparse_set<cell_id>`.

Two operations are needed and they pull in opposite directions:

- **O(1) membership**, for the transitive-marking early-out and for the edge
  filter during graph construction;
- **dense iteration over exactly the dirty cells**, because the recomputation
  builds one task per dirty cell and touching clean cells would defeat the whole
  design.

**Rejected — `std::set<cell_id>`:** gives membership in O(log n) and iteration
that chases red-black tree nodes. The iteration is the hot path here, and it is
exactly the access pattern Module 6 measured at 5.6× worse.

**Rejected — a `bool dirty` on each cell:** gives O(1) membership and *no* way
to enumerate the dirty cells without scanning all of them. That turns every
recompute into O(total cells) regardless of how few changed — which is the one
thing the design exists to avoid.

The sparse set gives both, which is why it exists.

## Decision 2 — transitive marking is depth-first with the dirty set as the visited set

```cpp
void mark_dirty_transitively(cell_id id) {
    if(dirty.contains(id)) return;      // <-- the early-out
    dirty.push(id);
    for(auto dependent : cells[index_of(id)].dependents)
        mark_dirty_transitively(dependent);
}
```

**Rejected — marking without a visited set.** In a diamond-shaped graph a cell
is reachable by exponentially many paths. `dataflow_engine.cpp` builds a
12-level diamond stack — 4,096 distinct root-to-tip paths — and checks that
exactly 36 cells are marked. Without the early-out that walk visits 4,096 paths
instead of 36 nodes.

**Known limitation, stated rather than hidden:** the marking is *recursive*, so
a pathologically deep graph can overflow the stack. An explicit worklist would
fix it. Not done, because it is a real change in readability for a failure mode
this engine's intended graphs (wide, shallow) do not have — and pretending the
limitation is not there would be worse than the limitation.

## Decision 3 — edges only between cells that are *both* dirty

```cpp
for(auto input : cells[index_of(pending[pos])].inputs)
    if(dirty.contains(input))
        tasks[position_of(pending, input)].precede(tasks[pos]);
```

**Chosen:** a clean input is already correct, so a dirty cell needs no ordering
constraint against it.

**Rejected — edges to every input.** It looks safer and it is catastrophic: an
edge to a clean cell means that cell needs a task, which means *its* inputs need
tasks, and the "partial" recomputation is the full one wearing a disguise.

This is the single line that makes the design incremental, and it is why the
dirty set needs O(1) `contains` rather than only iteration.

## Decision 4 — a fresh task graph per recompute

**Chosen:** build a `taskflow` each time `recompute` is called.

**Rejected — cache the graph and invalidate it on topology change.** Faster, and
substantially more complex: the cached graph is keyed on the *dirty set*, not on
the topology, so it would have to be rebuilt whenever the set of dirty cells
changed — which is every update. Caching would only pay if the same cells were
dirtied repeatedly, which is a workload assumption this engine does not get to
make.

**The cost is real and is reported rather than buried.** The `recomputed` column
in the benchmark is the honest measure of the design's benefit; the time ratio
includes graph construction, which is overhead the full-recomputation baseline
does not pay. Quoting only the time ratio would flatter the design.

## Decision 5 — mutation and recomputation are not concurrent

**Chosen contract:** `set` and `recompute` must not overlap, and `set` calls must
not overlap each other. Recomputation *is* internally parallel.

**Rejected — a lock inside `set` and `recompute`.** It would make the API
thread-safe and would not make it *useful*: two concurrent `set` calls that
dirty overlapping subgraphs still need the application to decide what a
consistent snapshot means. A lock would provide the illusion of safety over an
unanswered question.

**Rejected — a lock-free dirty set.** The dirty set is written during marking
and read during graph construction, which are different phases. Making it
lock-free would buy concurrency between phases that must not be concurrent
anyway, for the reason above.

The contract is stated in the header, and the engine is TSan-clean under it. A
caller that needs concurrent mutation batches updates and calls `recompute`
once, which is what a reactive system usually wants regardless.

## Decision 6 — `set` to the current value is a no-op

A reactive graph spends most of its life receiving updates that change nothing.
Comparing first turns those into zero work instead of a full subgraph
invalidation. Checked in `dataflow_engine.cpp`.

The exact-equality comparison on `double` is deliberate: the question is "is this
the same value", not "is this close enough". A tolerance here would silently
suppress genuine small changes, which in a dataflow graph accumulate.

## Decision 7 — a newly created computed cell starts dirty

Not marking it is the easiest way to ship a cell whose value is silently `0.0`
until something upstream happens to change. Creation is the one moment the engine
knows for certain a value has never been computed.

## Numbers

See `modules/13-capstone/dataflow_bench.cpp`. Run it; the numbers are also
recorded in `modules/13-capstone/NOTES.md` with the machine they came from.

The measurement caveat that applies to everything concurrent in this repo
applies here too, and is the reason `nproc` is printed next to the results: this
machine has **one core**, so nothing here supports a claim about parallel
speedup. What it does support is the claim about *work done*, which is the
`recomputed` column and is machine-independent.

## Capstone artifact checklist

| Required | Where |
|---|---|
| `docs/design.md` with rejected alternatives | this file |
| a benchmark suite with numbers | `modules/13-capstone/dataflow_bench.cpp`, `NOTES.md` |
| a TSan-clean build for anything concurrent | `scripts/verify.sh` tsan leg; CI's blocking TSan job |
| a `static_assert` battery | `modules/13-capstone/dataflow_engine.cpp`, `modules/03-layout-economy/layout_assertions.cpp` |
