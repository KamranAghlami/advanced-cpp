# Module 0 — Set up your instruments first

The course's opening insistence: *you cannot learn from a codebase you can only
read.* Everything here is instrumentation, not technique.

| Deliverable | Where |
|---|---|
| both libraries build and run at the pinned commits | `entt_smoke`, `taskflow_smoke` |
| the machine is fit for what the repo assumes | `toolchain_report` |
| a scratch TU that rebuilds in seconds | `acpp_exercise()` in the root `CMakeLists.txt` |
| the instrument list | `docs/CLAUDE.md` "Toolchain on this machine" |
| the technique log | [`docs/notes.md`](../../docs/notes.md) |

---

## What the smoke tests are for

They are not "does it compile". Each one drives the exact machinery a later
module dissects, so that when a reimplementation misbehaves you already know the
reference works here:

- **`entt_smoke`** — `type_name` without RTTI and a compile-time `_hs` hash
  (Module 1), a registry/view round trip over sparse-set storage (Modules 5–7),
  an empty component composing in a view with no payload array (Modules 2–3), and
  a stale handle failing validation (Module 5).
- **`taskflow_smoke`** — a diamond whose join counter must hold (Module 11), a
  partitioned `for_each` (Module 12), and `Executor::corun` executing a nested
  graph rather than parking a worker (Module 12).

One detail worth keeping: `Executor::corun(graph)` and `Runtime::corun()` are
different things in Taskflow 4.1 — the latter waits on tasks spawned by that
runtime and takes no argument. The widely-published examples predate it.

## `toolchain_report`

Added after the fact, once there was something to check *against*. It prints the
core count, compiler, sanitizer state, pointer width, endianness and cache-line
size — and **fails** if the machine is missing something the repo silently
assumes:

- `CHAR_BIT == 8`, because Modules 5 and 11 pack bitfields by hand;
- `std::atomic<int64_t>`, `<uint64_t>`, `<T*>` and `<size_t>` are **lock-free**.

That second one is the important one. `std::atomic` falls back to a lock table
for types the platform cannot do natively, and a "lock-free" work-stealing queue
silently backed by a mutex would pass **every test in this repo** while
invalidating everything Module 9 claims about it. Failing loudly beats measuring
quietly.

It also prints the core count with a pointed note when it is 1, because that
single number determines whether any concurrency measurement taken on the machine
is worth recording. See [`docs/pending-verification.md`](../../docs/pending-verification.md).

## What changed here after the rest of the course was written

Module 0 was built before the facilities the later modules produced, and was
retrofitted onto them:

- both smoke tests hand-rolled a `check()` and a `failures` counter; they now use
  `acpp::testing::suite` like every other exercise, which also gets them
  unbuffered output — a hung test that reports nothing is the least useful
  failure mode a harness has, and Phase C hit it twice;
- the `CMakeLists.txt` used `acpp_exercise()` plus a separate `add_test()`; it now
  uses `acpp_test()`, which is the same thing in one line.

Worth noting as a pattern rather than a chore: **the instruments are written
before you know what you need to measure, so expect to come back.** The
`toolchain_report` checks exist because Modules 9–11 turned out to depend on
lock-free atomics; nobody would have guessed that list up front.

## The instruments, and their state on this machine

The full table is in [`docs/CLAUDE.md`](../CLAUDE.md). The two entries that
shaped the most work:

- **`perf stat` is installed but unusable** (`perf_event_paranoid = 4` needs
  `CAP_PERFMON`). Module 6's cache measurements went through
  `valgrind --tool=cachegrind --cache-sim=yes` instead — slower, deterministic,
  and the substitute the course itself names.
- **One core.** Which is why `docs/pending-verification.md` exists.

Two instruments the course lists were replaced rather than skipped, and the
reasoning is recorded where each was used:

- `-ftime-trace` (clang) → bisecting the minimum `-ftemplate-depth` that
  compiles, in `cmake/measure_template_cost.cmake`. It answers "how deep?"
  directly, where a flame graph answers "where did the time go?".
- Compiler Explorer → `acpp_asm_probe()` + `cmake/check_asm.cmake`, which compile
  a TU to assembly at `-O2` and pattern-match one function's body. CI re-runs
  them; a browser tab does not.
