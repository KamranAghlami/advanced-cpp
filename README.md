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
| 3 | Layout economy | |
| 4 | The freestanding shim | |
| 5 | Bit-packed handles | |
| 6 | The sparse set | |
| 7 | Storage & view iteration | |
| 8 | Type erasure, three ways | |
| 9 | Chase–Lev work-stealing deque | |
| 10 | Sleeping without lost wakeups | |
| 11 | Scheduler & graph representation | |
| 12 | Parallel algorithm design | |
| ★ | Capstone | |

Done means the [Appendix C](docs/advanced-cpp-via-entt-and-taskflow.md#appendix-c--self-assessment)
checklist, not "it compiles."

## CI

[`ci.yml`](.github/workflows/ci.yml) builds and tests on every push and pull request:

- **build** — gcc and clang × C++20 and C++23, then `ctest`.
- **sanitizers** — ASan+UBSan (blocking) and TSan (informational for now, since the only
  threaded code in the tree is Taskflow's own; it becomes blocking when Module 9 lands).

Two environment notes that bit here and are worth knowing before Phase C: the sanitizer
runtimes need reduced ASLR entropy to start at all (`setarch $(uname -m) -R`, or
`vm.mmap_rnd_bits=28`), and **GCC's TSan silently ignores `atomic_thread_fence`** — the exact
instruction Modules 9 and 10 are built on. Use clang for anything thread-sanitized.
[`docs/CLAUDE.md`](docs/CLAUDE.md) has the full toolchain state.

## License

MIT — see [LICENSE](LICENSE). EnTT and Taskflow are MIT too, and are fetched rather than
vendored.
