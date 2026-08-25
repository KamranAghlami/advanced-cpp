# Capstone — a reactive dataflow engine

Option B from the course's capstone list. Design and rejected alternatives are in
[`docs/design.md`](../../docs/design.md); this file is the results.

Implementation: `src/acpp/dataflow.hpp`.
Correctness: `dataflow_engine.cpp`. Numbers: `dataflow_bench.cpp`.

---

## What it uses from the course

| Piece | Module | Doing what |
|---|---|---|
| `cell_id` (an enum over `uint32_t`) | 5 | a generational handle; the traits come for free |
| `basic_sparse_set<cell_id>` | 6 | the dirty set: O(1) membership *and* dense iteration |
| `bounded_wsq<node *>` | 9 | per-worker queues inside the executor |
| `nonblocking_notifier` | 10 | workers sleep between recomputes |
| `taskflow` + `executor` | 11 | the partial topological execution |

Not five pieces bolted together: the sparse set's `contains` is what makes
transitive marking linear instead of exponential, and the executor's join
counters are what make the recomputation parallel without a scheduler of its own.

## Correctness

`dataflow_engine.cpp`, all checks passing:

- **Only the affected subgraph is dirtied.** Two independent 10-cell chains from
  two inputs; touching one dirties exactly 10 cells and leaves the other chain
  untouched, and the recompute reports 10 of 22 cells.
- **A diamond is visited once.** 12 stacked diamonds — 4,096 distinct root-to-tip
  paths — mark exactly 36 cells. Without the sparse set's `contains` early-out
  that walk is exponential.
- **Partial equals full.** A random 120-cell DAG driven by 60 random updates,
  compared against a second engine doing full recomputation at every step. They
  agree to 1e-9 throughout, and the incremental one does strictly less work.
- **No-op updates are free.** Setting a cell to the value it already holds
  dirties nothing and recomputes nothing.

## Numbers

Layered DAG, each cell depending on two cells from the layer below, ~200 flops
per cell. gcc `-O2`, best of 5, one input touched per pass. Milliseconds:

| layers | width | cells | incr-par | incr-ser | full-ser | recomputed |
|---:|---:|---:|---:|---:|---:|---:|
| 6 | 40 | 240 | 0.100 | **0.045** | 0.688 | 64 |
| 12 | 40 | 480 | 0.873 | **0.704** | 1.513 | 295 |
| 20 | 60 | 1200 | 4.470 | **3.201** | 3.945 | 867 |

Three legs, because two would confuse two different effects:

**Work avoided** — `incr-ser` against `full-ser`:

| cells | affected fraction | speedup |
|---:|---:|---:|
| 240 | 27% | **15.3×** |
| 480 | 61% | 2.1× |
| 1200 | 72% | 1.23× |

Exactly the shape the design predicts: the benefit is the reciprocal of the
affected fraction, and it collapses as the update touches more of the graph. A
reactive engine is worth building when updates are *local*; when every update
dirties most of the graph it is overhead with extra steps.

**Parallel execution** — `incr-par` against `incr-ser`: **negative at every
size**. On one core there is no parallelism to win, and `incr-par` additionally
pays to build a task graph per recompute. That cost is real and is the subject of
Decision 4 in `docs/design.md`; it is reported rather than buried, because the
version of this table with only `incr-par` and `full-ser` would have looked like
a 15× win at the small size and a **loss** at the large one, and explained
neither.

The `recomputed` column is the only machine-independent number here, and it is
what the design actually optimises.

## Two bugs the benchmark had before it said anything true

1. **Passes that dirtied nothing.** The driver set random values, and a value a
   cell already held is correctly a no-op — so those passes took ~0 ms and won
   the "best of 5". The benchmark reported 0.000 ms for a recomputation that
   never happened. Now every pass sets a value the cell does not hold, and a
   pass that dirties nothing prints a warning instead of contributing.
2. **Inputs with no dependents.** Layer-1 cells picked both parents at random, so
   some layer-0 inputs were dead ends. Touching one dirtied nothing. The builder
   now assigns one parent round-robin, guaranteeing every cell in a layer has at
   least one dependent.

Both produced *plausible* numbers, which is what makes them worth recording: a
benchmark that crashes gets fixed, and a benchmark that quietly measures nothing
gets quoted.

## Capstone artifacts

| Required | Where |
|---|---|
| `docs/design.md` with rejected alternatives | [`docs/design.md`](../../docs/design.md) — 7 decisions |
| a benchmark suite with numbers | above, from `dataflow_bench.cpp` |
| TSan-clean for anything concurrent | `scripts/verify.sh` tsan leg; CI's blocking TSan job |
| a `static_assert` battery | `dataflow_engine.cpp` (handle layout), `modules/03-layout-economy/layout_assertions.cpp` |

## Known limitations, stated

- **Transitive marking is recursive**, so a pathologically deep graph can
  overflow the stack. An explicit worklist would fix it; not done, and the reason
  is in `docs/design.md`.
- **`set` and `recompute` must not overlap**, and neither may two `set` calls.
  The recomputation itself is parallel. That is a contract, and the reasoning for
  choosing it over a lock is in `docs/design.md`.
- **A fresh task graph is built per recompute.** Measurable, measured, and
  discussed rather than hidden.
