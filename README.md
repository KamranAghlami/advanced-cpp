# advanced-cpp

[![ci](https://github.com/KamranAghlami/advanced-cpp/actions/workflows/ci.yml/badge.svg)](https://github.com/KamranAghlami/advanced-cpp/actions/workflows/ci.yml)

A practice workspace for [**Advanced C++ by Dissection: EnTT + Taskflow**](docs/advanced-cpp-via-entt-and-taskflow.md)
— a 12-module course that reads two header-only libraries as primary sources and then
rebuilds their techniques from scratch.

EnTT is the compile-time machinery and memory-layout half: `consteval` type identity without
RTTI, traits as an API surface, EBO, sparse sets, paged storage, type erasure. Taskflow is the
concurrency half: a Chase–Lev work-stealing deque, a lost-wakeup-free notifier, a task-graph
scheduler, partitioned parallel algorithms.

This repo holds *my* implementations, not theirs. The two libraries are fetched to be read and
compared against.

## Quick start

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug   # first run clones the dependencies
cmake --build build -j
ctest --test-dir build --output-on-failure
```

CMake fetches EnTT and Taskflow at the exact commits the course text was validated against, so
its file and line references match. Sources land in `third_party/` (gitignored) rather than
`build/_deps`, so they survive `rm -rf build` — most of the course is spent reading them.

Build against the libraries' actual floor with `-DCMAKE_CXX_STANDARD=20`; the default is 23.

## Layout

```
CMakeLists.txt            root: dependencies, acpp_exercise(), acpp_asm_probe()
cmake/dependencies.cmake  FetchContent pins for EnTT + Taskflow
cmake/check_asm.cmake     pattern-checks one function's -O2 assembly
cmake/check_compile_fails.cmake  asserts a TU is ill-formed, for the right reason
cmake/measure_template_cost.cmake  bisects a metaprogram's instantiation depth
scripts/verify.sh         runs the CI matrix + sanitizers locally
src/acpp/                 the library the modules build up, linked as `acpp`
modules/NN-slug/          per-module exercises + NOTES.md, one executable each
docs/                     the course, the project guide, notes, memory
third_party/              gitignored; the two reference checkouts
```

The course is cumulative — Module 11's executor is assembled from Module 9's queue and Module
10's notifier — so anything a later module reuses lives in `src/acpp/`.

The course repeatedly asks for claims to be checked on Compiler Explorer. Since CI has no
Compiler Explorer, the claims that matter are compiled to assembly at `-O2` by
`acpp_asm_probe()` and pattern-checked by `cmake/check_asm.cmake`, so they are re-verified on
every build rather than asserted once in prose.

## Progress

| # | Module | Status |
|---|---|---|
| 0 | Tooling setup | ✅ [notes](modules/00-setup/NOTES.md) |
| 1 | Type identity without RTTI | ✅ [notes](modules/01-type-identity/NOTES.md) |
| 2 | Traits as an API surface | ✅ [notes](modules/02-traits/NOTES.md) |
| 3 | Layout economy | ✅ [notes](modules/03-layout-economy/NOTES.md) |
| 4 | The freestanding shim | ✅ [notes](modules/04-freestanding-shim/NOTES.md) |
| 5 | Bit-packed handles | ✅ [notes](modules/05-handles/NOTES.md) |
| 6 | The sparse set | ✅ [notes](modules/06-sparse-set/NOTES.md) |
| 7 | Storage & view iteration | ✅ [notes](modules/07-storage-and-views/NOTES.md) |
| 8 | Type erasure, three ways | ✅ [notes](modules/08-type-erasure/NOTES.md) |
| 9 | Chase–Lev work-stealing deque | ✅ [notes](modules/09-work-stealing-deque/NOTES.md) |
| 10 | Sleeping without lost wakeups | ✅ [notes](modules/10-notifier/NOTES.md) |
| 11 | Scheduler & graph representation | ✅ [notes](modules/11-scheduler/NOTES.md) |
| 12 | Parallel algorithm design | ✅ [notes](modules/12-parallel-algorithms/NOTES.md) |
| ★ | Capstone — reactive dataflow engine | ✅ [notes](modules/13-capstone/NOTES.md) · [design](docs/design.md) |

Done means the [Appendix C](docs/advanced-cpp-via-entt-and-taskflow.md#appendix-c--self-assessment)
checklist, not "it compiles." Where each item is answered:

| Appendix C item | Answered in |
|---|---|
| Why `type_index` needs visibility control across shared objects | [M1](modules/01-type-identity/NOTES.md) — plus a finding the course text does not mention |
| The three-level customization ladder, from memory | [M2](modules/02-traits/NOTES.md) |
| The exact code that breaks a tag-less `compressed_pair` | [M3](modules/03-layout-economy/NOTES.md) — compiled, and required to fail |
| The standard-library surface a nontrivial library actually needs | [M4](modules/04-freestanding-shim/NOTES.md) — 154 names, and the shape is the surprise |
| A bit split for a generational handle, given live-count and churn | [M5](modules/05-handles/NOTES.md) — 24/40, with the arithmetic |
| A sparse set with holes, including tombstone contents | [M6](modules/06-sparse-set/NOTES.md) |
| Paged sparse vs paged payload — different motivations | [M7](modules/07-storage-and-views/NOTES.md) |
| `std::function` vs delegate vs virtual vs raw pointer | [M8](modules/08-type-erasure/NOTES.md) — three follow-up questions |
| Every memory order in Chase–Lev `pop` and `steal` | [M9](modules/09-work-stealing-deque/NOTES.md) — all four questions, in writing |
| The lost-wakeup window and the instruction that closes it | [M10](modules/10-notifier/NOTES.md) — two-column timeline |
| A task node's full lifecycle, with every atomic named | [M11](modules/11-scheduler/NOTES.md) |
| Load imbalance from the wrong partitioner | [M12](modules/12-parallel-algorithms/NOTES.md) — and why this machine cannot test it |

## What the measurements actually said

Several results contradict what the course text predicts, and those are the ones worth
reading:

- **Module 8** — "delegates inline, `std::function` does not" is false for a local
  `std::function` at `-O2`; gcc devirtualises both. The delegate's edge is size, allocation
  and the absent empty-target check.
- **Module 11** — `std::visit` compiled to *fewer* instructions than switch-on-index, because
  it is exhaustive by construction and emits no `default` case.
- **Module 9** — the deliberately-weakened memory order *passes* the stress test on x86.
  **Settled on an M1 (2026-08-26) for the ARM half, and settled again on 16-core x86 WSL2
  (2026-08-27) for the x86 half — the two results together are the finding.** The weakened
  *fence* cannot fail on ARM: clang lowers `seq_cst` and `acq_rel` fences to the same
  `dmb ish`, so that build is not weakened at all there. Moving the knob to the publish store
  (`release` → `relaxed`, a real `stlr` → `str`) and holding the queue shallow enough for the
  race window to open: **156 failures in 200 runs, control 0 in 200** on the M1. **The fence
  knob, believed vacuous on x86 too, turned out only to be untested there: 16 real cores
  under clang fail it 199 times in 200** (control 0/200) — clang really does drop the barrier
  for `acq_rel` on x86-64, exactly as its own codegen comment predicted, and no machine before
  this one could supply both x86 *and* real parallelism at once. A second finding fell out
  while confirming it at the instruction level: **gcc's x86-64 codegen never distinguishes
  the two fence strengths either** (`lock orq` present in both builds) — a second vacuous
  pairing, on a compiler axis rather than an ISA one (0/50 failures under gcc, same box). TSan
  saw none of either bug — every access is a `std::atomic`, so there is no data race, only an
  insufficient protocol. `wsq_weakened` ran unfiltered in CI's blocking `build` job under
  clang until this was caught; it is now build-only (`acpp_exercise`, matching its sibling
  `wsq_weakened_release`), so it can no longer gate CI. Checked against CI history: it never
  actually caused a failure in the ~18 clang-leg runs before the fix — see
  `modules/09-work-stealing-deque/NOTES.md`, "The fence half found its machine too".
- **Module 10** — spinning costs ~79× the idle CPU of any sleeping strategy. On the 1-vCPU
  droplet the two-phase notifier did **not** beat a condvar on idle CPU, and the prediction
  was that its advantage needs a lock-free push path to appear at all. **On 16 real cores
  (2026-08-27) it does beat condvar — 4× lower CPU — without the push path becoming
  lock-free.** The mechanism is the 2PC guard's park-avoidance check catching far more cases
  under real concurrency (63–89 parks out of 480 pushes, down from 372 on one core); condvar
  still signals on every push by construction, and that fixed cost is what real cores expose.
- **Module 12** — the partitioner prediction ("each partitioner wins one workload") was
  untested on one core, not disproved. **Tested on 16 real cores: it still does not hold** —
  guided wins uniform, proportional, *and* heavy-tail, and this time the ranking is real
  (run-to-run spread is ≤0.15 ms against multi-ms gaps between workloads).
- **Module 9's scaling curve** — also only answerable on real cores. Chase–Lev vs.
  `std::mutex` + `std::deque` go from parity at 1 thread to 20.4× at 16: the mutex serialises
  regardless of core count (30 ms → 557 ms) while Chase–Lev's cost under real `top`-side
  contention rises far more slowly (5 ms → 27 ms).
- **Module 8's erasure-mechanism ranking** — unresolved on a shared vCPU (noise exceeded the
  gaps between mechanisms). On a quiet 16-core box it resolves into three tiers: raw pointer ≈
  virtual call < delegates < `std::function`/`poly`, with per-row noise now an order of
  magnitude below the gaps between tiers.

The development machine has **one core** and is x86, so every concurrency measurement taken
there reports `nproc` alongside it and none of them supports a scaling claim. As of
2026-08-26 the memory-order arguments *have* been run on hardware that can falsify them: an
8-core M1 caught the Module 9 weakening above, and found two more things on the way — a
Module 1 cross-DSO bug where `-fvisibility-inlines-hidden` silently gave each shared object
its own type-id counter (two component types aliasing one storage slot, with the test green),
and two `#include`s that only libstdc++ was supplying transitively. As of 2026-08-27 a
16-core x86 WSL2 box (i9-9900K) settled every throughput and scaling number the single core
could only flag as untested — see the Modules 8, 9, 10 and 12 bullets above — running the full
`ctest` suite and 200 iterations of the concurrency set clean, plus all six `verify.sh` legs
including TSan.

The general rule that fell out of it: **x86 and ARM catch complementary halves of the memory
model.** x86 lowers acquire/release loads and stores to plain `mov` but distinguishes the two
fences; ARM is the exact mirror. Neither machine alone tests both, and a weakening experiment
has to be aimed at the half the machine can see.

The multi-machine handoff that used to live in `docs/pending-verification.md` is closed as of
2026-08-27 — every measurement it tracked has a result (a number, or, for the cache-line-size
throughput question, a considered "not established"), and the `wsq_weakened` CI risk it turned
up is fixed. Its results are folded into this section and into each module's `NOTES.md`; the
file itself is retired rather than kept as a growing pile of past handoffs. Start a new machine
with `./build/modules/00-setup/toolchain_report`, which prints the core count and cache-line
size and fails if the atomics this repo assumes are not lock-free.

## CI

[`ci.yml`](.github/workflows/ci.yml) builds and tests on every push and pull request:

- **build** — gcc and clang × C++20 and C++23, then `ctest`.
- **sanitizers** — ASan+UBSan and TSan, both blocking. TSan became blocking with Module 9,
  when the first lock-free code of our own landed in `src/`.

Two environment notes that bit here and are worth knowing before Phase C: the sanitizer
runtimes need reduced ASLR entropy to start at all (`setarch $(uname -m) -R`, or
`vm.mmap_rnd_bits=28`), and **GCC's TSan silently ignores `atomic_thread_fence`** — the exact
instruction Modules 9 and 10 are built on. Use clang for anything thread-sanitized.
[`docs/CLAUDE.md`](docs/CLAUDE.md) has the full toolchain state.

## License

MIT — see [LICENSE](LICENSE). EnTT and Taskflow are MIT too, and are fetched rather than
vendored.
