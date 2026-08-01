# docs/CLAUDE.md — project guide

## What this repo is

A **practice workspace**, not a product. It exists to work through the 12-module course in
[`docs/advanced-cpp-via-entt-and-taskflow.md`](advanced-cpp-via-entt-and-taskflow.md), which
teaches advanced C++ by dissecting two header-only libraries (EnTT and Taskflow) and then
re-implementing their techniques from scratch.

That document is the **source of truth** for what gets built here. Read the relevant module
before writing code for it — every exercise in this repo traces back to a numbered exercise
there. Module 0 (tooling setup) is done — the dependencies are wired up and both smoke tests
pass. Module 1 is the next thing to start. Everything else gets added as modules are worked
through.

Success criteria for the whole project are Appendix C of the course doc (the self-assessment
checklist), not "the code compiles."

## Reference libraries

Fetched by CMake at configure time — see [`cmake/dependencies.cmake`](../cmake/dependencies.cmake).
No manual cloning; `cmake -S . -B build` is enough.

They are pinned to the **full SHAs** of the commits the course was written and validated
against, so its file/line references match what you read:

| | commit | note |
|---|---|---|
| EnTT | `85c6bba0140…` (2026-07-22) | master toward v4; `version.h` reads 4.0.0, untagged |
| Taskflow | `c4da2a49cd8…` (2026-07-28) | `TF_VERSION 400100` → 4.1.0 |

Do not bump these casually — the document's quoted fragments are only guaranteed correct here.

Sources land in `third_party/` (gitignored) rather than the default `build/_deps`, so they
survive `rm -rf build`; this course is mostly about *reading* them. Taskflow's
`TF_BUILD_TESTS`/`TF_BUILD_EXAMPLES` default to ON and are forced OFF before
`FetchContent_MakeAvailable` — configure the `third_party/taskflow` checkout separately to
build those for study.

Exercise code for Modules 1–12 should generally **not** link `EnTT::EnTT` or
`Taskflow::Taskflow`. The point is to reimplement their techniques; the dependencies are here
to be read, built, and compared against.

When answering a question about how EnTT or Taskflow works, **read the pinned source** rather
than recalling it. The course doc explicitly warns about traps like the two definitions of
`_explore_task` in `taskflow/core/executor.hpp` (one live, one commented out and subtly
different) — `grep -n` for a symbol and check the hit count before studying it. The v4-dev
EnTT and 4.1 Taskflow APIs also differ from the widely-documented releases (e.g.
`Runtime::corun()` takes no argument in 4.1; nested graphs go through `Executor::corun`).

## Layout

```
README.md               human-facing intro + module progress table
CMakeLists.txt          root: deps, the template executable, acpp_exercise()
cmake/dependencies.cmake  FetchContent pins for EnTT + Taskflow
.github/workflows/ci.yml  build matrix + sanitizers
src/                    shared, reusable code: the pieces later modules build on
modules/NN-slug/        per-module exercise programs + CMakeLists.txt + NOTES.md
  00-setup/             Module 0 smoke tests (entt_smoke, taskflow_smoke)
docs/                   course doc, this guide, notes.md, memory/
build/                  gitignored (pattern is case-insensitive)
third_party/            gitignored; entt + taskflow checkouts, managed by FetchContent
```

The course is **cumulative**: Module 11's executor is built from Module 9's work-stealing
queue and Module 10's notifier; Module 7's storage extends Module 6's sparse set. So anything
a later module reuses belongs in `src/` under namespace `acpp::`, with the per-module
executables in `modules/` including it. Exercise code that is purely throwaway
(Compiler Explorer-style probes) stays in the module directory.

Each exercise gets its own executable, since each has a `main()`:

```cmake
acpp_exercise(sparse_set_invariants sparse_set_invariants.cpp)   # in modules/06-sparse-set/
```

`acpp_exercise()` is defined in the root `CMakeLists.txt` and puts `src/` on the include path.
Add each new `modules/NN-slug/` with an `add_subdirectory` line in the root list file.

`src/main.cpp` is still the template's hello-world, built as target `advanced-cpp` from a
`GLOB_RECURSE` over `src/*.cpp`. When `src/` becomes the shared library it is meant to be,
that glob-into-an-executable has to become a library target — do it deliberately, not as a
drive-by.

## Build & run

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug   # first run clones the deps
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Anything that should gate CI gets an `add_test(NAME x COMMAND x)` next to its
`acpp_exercise()`. Benchmarks and codegen probes should *not* — they are not pass/fail.

The smoke tests also print per-check `ok`/`FAIL` lines and exit non-zero on failure, so they
work as a standalone readiness check after a toolchain or pin change.

C++ standard defaults to **23** but is a cache variable, so `-DCMAKE_CXX_STANDARD=20` works.
The libraries under study require **C++20** and use C++20 spellings throughout (`requires`,
`consteval`, `using enum`, `std::popcount`, `std::bit_ceil`, `atomic_flag::test`). When
reproducing a library technique, confirm it still compiles at 20 — otherwise you have learned
a C++23 solution to a C++20 problem. CI enforces this.

## CI

[`.github/workflows/ci.yml`](../.github/workflows/ci.yml), on push to `main`, on PRs, and on
manual dispatch:

- **build** — gcc × clang × C++20 × C++23, then `ctest`.
- **sanitizers** — clang, ASan+UBSan blocking and TSan `continue-on-error`.

The TSan leg is informational **only because there is no concurrent code of ours yet** — the
only threaded code in the tree is Taskflow's own lock-free internals. Flip
`informational: false` on that matrix entry when Module 9's queue lands in `src/`; leaving it
informational past that point would quietly defeat the Phase C ground rules.

Both sanitizer legs need `vm.mmap_rnd_bits=28` (the workflow sets it) for the same ASLR reason
described below.

Reference builds of the libraries themselves (useful for reading their tests):

```bash
cmake -S third_party/entt     -B third_party/entt/build     -DENTT_BUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Debug
cmake -S third_party/taskflow -B third_party/taskflow/build -DTF_BUILD_TESTS=ON -DTF_BUILD_EXAMPLES=ON
```

## Toolchain on this machine (verified 2026-08-01)

| Tool | Status |
|---|---|
| `g++` 13.3 (Ubuntu 24.04), `cmake` 3.28.3, `ninja`, `perf` | available |
| ASan + UBSan (`-fsanitize=address,undefined`) | works |
| **TSan** (`-fsanitize=thread`) | two caveats, both verified — see below |
| `clang++` / `clang-tidy` / `-ftime-trace` | **not installed** — the course's `-ftime-trace` instructions (Modules 2, 8) need clang |
| `valgrind` / `cachegrind` | **not installed** — cache modelling exercises (Module 6) need it, or use `perf stat -e cache-misses` |
| `gdb` / `lldb` | **not installed** |
| Google Benchmark, Catch2 | **not installed** — fetch via CMake `FetchContent` when a module needs them |

### TSan caveats (Phase C blockers)

1. **It aborts at startup** with `FATAL: ThreadSanitizer: unexpected memory mapping` — the
   ASLR/`vm.mmap_rnd_bits` conflict. Run under disabled ASLR:
   `setarch $(uname -m) -R ./binary`. Verified working; `taskflow_smoke` passes that way.
2. **GCC's TSan does not model `atomic_thread_fence`.** Building anything that uses one emits
   `warning: 'atomic_thread_fence' is not supported with '-fsanitize=thread' [-Wtsan]`, and
   the fence is then ignored by the race detector. Modules 9 and 10 rest *entirely* on
   `std::atomic_thread_fence(seq_cst)` — it is the instruction that closes the Chase–Lev
   `pop`/`steal` race and the lost-wakeup window. So GCC TSan cannot validate the exact thing
   those modules teach, and a clean run there proves less than it appears to.
   **Install clang before Module 9** and treat any GCC-only TSan result as provisional.

`/proc/cpuinfo` reports **1 CPU**. Phase C (Modules 9–12) is about work stealing across
threads — steal-rate, idle-power, and thread-scaling measurements are meaningless on one core.
Check `nproc` before trusting any concurrency benchmark, and say so in the numbers you record.

Module 0 of the course insists on setting the instruments up *before* Module 1. Installing
clang and valgrind is part of that; flag the gap rather than silently skipping an exercise
that depends on them.

## Conventions for exercise code

- **Measure, don't assert.** The course repeatedly says "verify this yourself on Compiler
  Explorer" / "numbers you measured, not numbers you expect". Never claim something compiles
  to zero instructions, inlines, or is faster without having run it. If a claim is unverified,
  label it as a prediction.
- **`static_assert` batteries.** Layout and trait assumptions (`sizeof`, `alignof`, `offsetof`,
  `is_standard_layout_v`) get asserted in code, per Modules 3 and 7 and the capstone artifact list.
- **Everything concurrent is TSan-clean** before it is believed (Phase C ground rules). On this
  machine that means `setarch $(uname -m) -R`.
- **Write down decisions with their rejected alternative.** Several exercises (version
  wraparound in Module 5, multiple simultaneous exceptions in Module 11) have no universally
  right answer — the deliverable is a documented decision, not a specific behaviour.
- `docs/notes.md` is the technique log the course asks for in Module 0: one entry per
  technique — *name, file, one-sentence mechanism, where I'd use it*. Per-module written
  answers (e.g. the four memory-order questions in §9.2) go in `modules/NN-slug/NOTES.md`.

## How to help here

The point of this repo is that **Kamran writes the exercise code**. Handing over a finished
sparse set or Chase–Lev deque destroys the exercise. Default behaviour:

- **Do**: explain a technique in the pinned source, locate symbols, review code that's already
  written, build/run/measure things, set up CMake and test harnesses, build minimal probes that
  answer a codegen question, and check answers against the real implementation.
- **Ask first**: writing a full exercise solution. If asked outright, write it — but say what
  the exercise was meant to teach.
- **Never**: pre-empt an exercise by dropping the answer into a review or an explanation of a
  neighbouring module.

Say so if this default should change — it's one section in this file.

## Course map

Detail lives in the course doc; this is for locating a module fast.

| # | Module | Primary source |
|---|---|---|
| 0 | Tooling setup | — |
| 1 | Type identity without RTTI | `entt/core/type_info.hpp`, `core/hashed_string.hpp`, `config/config.h` |
| 2 | Traits as an API surface | `entt/entity/component.hpp`, `core/type_traits.hpp` |
| 3 | Layout economy (EBO) | `entt/core/compressed_pair.hpp` |
| 4 | The freestanding shim | `entt/stl/` |
| 5 | Bit-packed handles | `entt/entity/entity.hpp` |
| 6 | The sparse set | `entt/entity/sparse_set.hpp`, `core/bit.hpp` |
| 7 | Storage & view iteration | `entt/entity/storage.hpp`, `entity/view.hpp` |
| 8 | Type erasure, three ways | `entt/core/any.hpp`, `signal/delegate.hpp`, `poly/poly.hpp` |
| 9 | Chase–Lev work-stealing deque | `taskflow/core/wsq.hpp` |
| 10 | Sleeping without lost wakeups | `taskflow/core/nonblocking_notifier.hpp`, `core/executor.hpp` |
| 11 | Scheduler & graph representation | `taskflow/core/graph.hpp`, `core/executor.hpp`, `core/topology.hpp` |
| 12 | Parallel algorithm design | `taskflow/algorithm/partitioner.hpp`, `algorithm/pipeline.hpp`, `core/runtime.hpp` |
| ★ | Capstone | — |

Phases: **A** (1–4) compile-time machinery · **B** (5–8) data structure design ·
**C** (9–12) concurrency. Appendix A of the course doc is a technique→file index; Appendix B
is companion reading; Appendix C is the done-criteria checklist.
