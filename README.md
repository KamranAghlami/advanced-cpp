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
| 0 | Tooling setup | ✅ |
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
- **Module 9** — the deliberately-weakened memory order *passes* the stress test. A green
  test on x86 is not evidence that a memory order is correct.
- **Module 10** — spinning costs ~79× the idle CPU of any sleeping strategy, but the
  two-phase notifier did **not** beat a condvar on idle CPU; its advantage needs a lock-free
  push path to appear at all.
- **Module 12** — the partitioner prediction is untested, not disproved: with one core there
  is no imbalance for a partitioner to fix.

This machine has **one core**, so every concurrency measurement here reports `nproc`
alongside it and none of them supports a scaling claim.

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
