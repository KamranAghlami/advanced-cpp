# Pending verification — what still needs other hardware

**Status:** every module and the capstone is implemented, tested and CI-green.
What is *not* settled is a set of **measurements**, because the machine they were
taken on cannot produce the conditions they describe.

**Read this first if you are the agent or person running on another machine.**

> **Update 2026-08-26 — the M1 run happened.** §8a is **settled**, though not the
> way this document predicted: the weakened *fence* cannot fail on ARM either,
> because clang lowers `seq_cst` and `acq_rel` fences to the same `dmb ish`.
> Moving the knob to the publish store caught it, 156 failures in 200 runs. §8c
> is settled. Two bugs and one build break were found on the way. Rows **1–7
> remain open and still belong to the x86 box** — nothing below them has changed.
> Details in the §8 sections; the per-module results are in each `NOTES.md`.

---

## The machines, and what each one is actually for

Two are available beyond the development box, and they are **not ranked** — they
answer different questions, and the M1 answers one that no amount of x86 can.

| Machine | Good for | Not for |
|---|---|---|
| **16-core x86 WSL2**, 32 GB | every throughput and scaling measurement; `perf`; the fastest full `verify.sh` | the memory-model question (x86-TSO hides it) |
| **M1 MacBook Air (8 cores)** | **weak memory ordering** — the one thing x86 structurally cannot test; a second standard library (libc++); a different cache-line size | sustained benchmarking (fanless, it thermally throttles); cachegrind (unsupported on Apple Silicon); `perf` (does not exist) |
| single vCPU droplet (this one) | nothing further | everything below |

**If you only run one, run the M1 first.** The x86 box gives better numbers for
things that already have plausible numbers. The M1 can find *bugs*, and bugs
outrank numbers — see the next section for why that is not a hypothetical.

Running both is better still, and they do not conflict: they write to different
sections. See "Two machines at once" at the end.

---

## Why this exists

The development machine is a **single shared vCPU** (DigitalOcean droplet,
`nproc` = 1, ~2.3 GHz, with steal time). Three consequences ran through the whole
of Phase C:

1. **No parallelism.** Threads time-slice instead of running at once, so load
   imbalance costs nothing, steal rates reflect the OS scheduler rather than the
   heuristic, and "N threads" means oversubscription. Every concurrency number in
   this repo prints `nproc` next to it for exactly this reason.
2. **Noise exceeds signal.** On a shared vCPU the best-to-worst spread of one
   benchmark row was larger than the differences between rows.
3. **Real bugs stayed hidden.** Two defects passed every local test *and* a clean
   TSan run here, and were caught only by CI's four-core runners:
   - the pipeline's stop point was a `bool`, so a token still queued for a later
     stage when the source stopped was silently dropped;
   - `park()` erased a signal that arrived between publishing a waiter on the
     stack and taking its mutex — a lost wakeup **inside** the lost-wakeup fix,
     which TSan cannot see because every access is under a mutex.

If four cores found two bugs, sixteen may find more — and **an ARM machine may
find a different class entirely.** Every memory-order argument in Modules 9, 10
and 11 was written against the C++ model and then tested only on x86-TSO, which
provides most of that ordering in hardware whether or not you asked for it. A
`relaxed` that should have been `acquire` costs nothing on x86 and corrupts on
ARM. The repo has never once been run on a machine that could tell.

`perf stat` is also unusable here (`perf_event_paranoid = 4`), so cache work went
through cachegrind. WSL2 usually allows `perf`; macOS has neither.

## Setting up

**WSL2 (x86):** the usual.

```bash
sudo apt-get install -y build-essential clang cmake ninja-build valgrind linux-tools-generic
```

**macOS (M1):** Homebrew, and note what is *missing*.

```bash
brew install cmake ninja llvm        # llvm optional; Apple clang is fine and is what ships
```

- No `valgrind` (unsupported on Apple Silicon) → Module 6's cachegrind numbers
  cannot be re-taken there. Leave them alone.
- No `perf` → use `xcrun xctrace` / Instruments if you want counters, or skip.
- No `setarch` → not needed; `scripts/verify.sh` already skips it off Linux.
- **libc++, not libstdc++.** That is a feature, not a problem — see the ARM
  section.

**What actually broke on first contact (2026-08-26), now fixed — expect none of
this, but recognise it if a pin moves):**

1. **Two `#include`s were missing** — `<mutex>` in `modules/02-traits/` and
   `<algorithm>` in `modules/06-sparse-set/`. libstdc++ had been supplying both
   transitively. Pure portability lint; the fix is one line each.
2. **`odr_across_dso` failed**, and it was a real bug rather than a platform
   quirk — see the Module 1 note below.
3. **`scripts/verify.sh` aborted on its first leg** under macOS's bash 3.2,
   where an empty array expanded under `set -u` is an unbound variable. It exits
   non-zero but looks like a leg in progress, so check that six legs actually
   ran.
4. **`cmake` 4.x and Apple clang 21 both configure and build cleanly** otherwise,
   and the codegen assertions auto-skip with the status line promised above.

**Module 1's cross-DSO exercise needed a real fix, not a skip.** There is no
`STB_GNU_UNIQUE` on Mach-O; cross-image merging of a function-local static is
dyld coalescing `weak external` definitions, and `-fvisibility-inlines-hidden`
demotes them to `non-external`. The trap is what happened next: fixing it for the
two Module 1 targets made the test pass while `acpp`'s own counter stayed
per-image, so `alpha` and a plugin-registered type both held id **0** — two
component types aliasing one storage slot, test green. `VISIBILITY_INLINES_HIDDEN`
is now `OFF` on Apple for `acpp` too (`ACPP_INLINES_HIDDEN`), and macOS
reproduces the ELF numbers exactly. **Agreement and uniqueness are different
invariants; the test only checked the first.**
- The codegen assertions **auto-skip**: their patterns are x86-64 AT&T assembly
  with ELF symbol names, and CMake prints a status line saying so. The `.s` files
  are still generated.

## Before anything else, on either machine

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
./build/modules/00-setup/toolchain_report
```

That prints the core count, compiler, sanitizer state and cache-line size, and
**fails** if `std::atomic<int64_t>` / `<uint64_t>` / `<T*>` are not lock-free —
because a "lock-free" queue silently backed by a lock table would pass every test
in this repo while invalidating everything Module 9 claims.

Record its output; it is the header for every result below.

## Step 1 — correctness first, and repeatedly

The bugs matter more than the numbers.

```bash
ctest --test-dir build --output-on-failure -LE measurement       # all 61
./scripts/verify.sh                                              # gcc/clang x C++20/23 + ASan + TSan
```

`scripts/verify.sh` takes ~5 hours on the single-core box and should take minutes
on 16 cores. **Do not run it concurrently with another build of this project** —
every build tree shares `third_party/`, and FetchContent will wipe and re-clone it
underneath the other one.

Then hammer the concurrency tests, because a race that appears once in fifty runs
is exactly what this machine could not find:

```bash
for i in $(seq 1 200); do
  ctest --test-dir build --output-on-failure \
        -R 'wsq_stress|notifier_protocol|lost_wakeup|dag_executor|exception_propagation|corun_deadlock|pipeline_ordering|dataflow_engine' \
    || { echo "FAILED on iteration $i"; break; }
done
```

And the same under TSan, which on 16 cores explores far more interleavings:

```bash
sudo sysctl -w vm.mmap_rnd_bits=28      # or use setarch below instead
CXX=clang++ cmake -S . -B build/tsan -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="-fsanitize=thread -fno-omit-frame-pointer -g"
cmake --build build/tsan -j
for i in $(seq 1 20); do
  TSAN_OPTIONS=halt_on_error=1 setarch "$(uname -m)" -R \
    ctest --test-dir build/tsan --output-on-failure -LE measurement || break
done
```

**If anything fails, that is the most valuable result of the whole exercise.**
Capture the full output, fix or report it, and note it in the relevant module's
`NOTES.md` under the pattern the existing "what TSan found" sections use.

## Step 2 — the measurements to re-take

Each row says what to run, what the single-core result was, and what the open
question is. **Run each benchmark 3–5 times and use the best**, and record
`nproc` with every number.

### High value — these are currently *untested*, not merely imprecise

| # | Run | Single-core result | The open question |
|---|---|---|---|
| 1 | `./build/modules/12-parallel-algorithms/partitioner_bench` | spread ~10%, mostly noise | The course predicts **each partitioner wins one workload** (uniform / proportional / heavy-tail). Untested here — there was no imbalance to fix. Does the prediction hold? |
| 2 | `./build/modules/09-work-stealing-deque/wsq_bench` | 1.14× uncontended, "threads" column was oversubscription | The actual **scaling curve** at 1/2/4/8/16 threads against `std::deque` + `std::mutex`, which is exercise 5's real deliverable. |
| 3 | `./build/modules/11-scheduler/sticky_victim_bench` | 23% fewer steal attempts, 14.5%→19.8% hit rate | Does sticky victim still help with **real** stealing? The single-core numbers reflect the OS scheduler's interleaving as much as the heuristic. |
| 4 | `./build/modules/13-capstone/dataflow_bench` | `incr-par` **lost** to `incr-ser` at every size | Parallel recomputation should win on 16 cores. Where is the crossover — how large must the dirty subgraph be before parallel beats serial? |

### Medium value — a real result exists but is noise-limited

| # | Run | Single-core result | The open question |
|---|---|---|---|
| 5 | `./build/modules/10-notifier/notifier_idle` | spin 0.259 s CPU; 2PC 0.0033 s; **condvar 0.0015 s** | 2PC did *not* beat condvar-per-push on idle CPU, because the harness gives all three the same locked queue. With real contention, does the no-lock-on-push advantage appear? |
| 6 | `./build/modules/08-type-erasure/erasure_table` | ranking **not established** — spread within a row exceeded differences between rows | Does the ranking of raw pointer / virtual / delegate / `std::function` / `poly` resolve on a quiet machine? |
| 7 | `./build/modules/06-sparse-set/sparse_set_bench` | 5.6× over `unordered_map` at 1M; cachegrind for misses | Cross-check the cache-miss ratio with `perf stat -e cache-misses,cache-references,LLC-load-misses` if WSL2 allows it. Compare against the cachegrind numbers already recorded. |

### Still not answerable on WSL2 — but the M1 changes one of them

| # | What | Why |
|---|---|---|
| ~~8~~ | ~~Module 9 exercise 3 — does a weakened memory order get caught?~~ | **DONE 2026-08-26.** Yes, once the knob is aimed at the half of the memory model ARM can see. See §8a. |
| 9 | Module 3 — MSVC `[[no_unique_address]]` layout | Needs MSVC. Neither WSL2 nor macOS has it, but the **Windows host does** — `cl.exe /std:c++20 /EHsc` on `modules/03-layout-economy/compressed_pair_layout.cpp` would settle it. |

## Step 2b — the M1's job: weak memory ordering

This is the highest-value item in the whole document, because it is the only one
where the current answer is *"we could not test this"* rather than *"the number
is imprecise"*.

### 8a. Does the deliberately-weakened fence get caught? — **SETTLED 2026-08-26**

**No, and it never could have been.** The premise this section was written on is
wrong, and the way it is wrong is the finding.

Clang lowers *both* fence strengths to the same instruction on AArch64:

```
                        AArch64          x86-64
seq_cst fence           dmb ish          lock orl $0, -64(%rsp)
acq_rel fence           dmb ish          <nothing at all>
```

`dmb ish` is a full barrier, store-load included. So `-DACPP_WSQ_WEAKEN_FENCE`
produces **identical barrier codegen** on ARM: `wsq_weakened` is not a weakened
binary. 500 runs, 0 failures — and that is *no* evidence rather than weak
evidence. The hardware does reorder; the compiler never gives it the chance.

Note the direction. On **x86** the weakening is a real codegen change (the
barrier vanishes) and the test passed anyway. On **ARM** the test cannot fail.
The platform this item was waiting for is the one where the experiment is
vacuous.

**The general rule**, which is what to carry forward:

```
                        AArch64          x86-64
relaxed / acquire load  ldr / ldapr      movq / movq
relaxed / release store str / stlr       movq / movq
seq_cst vs acq_rel      same             differs
```

x86 catches a weakened **fence**; ARM catches a weakened **load/store order**.
Complementary halves — aim the experiment at the half the machine can see.

### 8a-bis. The knob that does work — **156/200, control 0/200**

`ACPP_WSQ_WEAKEN_RELEASE` demotes `push`'s publishing store on `bottom` from
`release` to `relaxed` (a real `stlr` → `str`; verified in the object file before
being trusted).

**`wsq_stress` still could not catch it — 500 runs, 0 failures.** The harness was
the problem: it pushes three items per pop, so the queue grows to 65,536 slots
and thieves work at `top` while the owner works at `bottom`, thousands of slots
apart. The publish race needs them on the same slot.

`modules/09-work-stealing-deque/wsq_publish_race.cpp` holds the queue 0–2 deep.
Apple clang 21, arm64, 8 cores, `-O2`:

| build | runs | failures |
|---|---:|---:|
| `wsq_publish_race` (control) | 200 | **0** |
| `wsq_publish_race_weakened` | 200 | **156** |

First failure on run 1. `duplicated` and `lost` are always equal — the thief
reads the slot before the owner's write lands, consuming the previous
generation's pointer twice and the new item never.

**TSan reports nothing**: 15 runs, 14 corrupted, 0 race reports. Every access is
a `std::atomic` with an explicit order, so there is no data race — only an
insufficient protocol. Second instance in this repo after Module 10's `park()`.

*If you are on the x86 box:* the control build is now a CI test and must pass.
The weakened build is expected to **pass** there, which is the point — it is the
half x86 cannot see.

### 8b. Everything else that rests on a memory order

Modules 9, 10 and 11 are full of `relaxed` loads justified by an argument. ARM is
where a wrong one bites. Run the whole concurrency set hard:

```bash
for i in $(seq 1 500); do
  ctest --test-dir build --output-on-failure \
    -R 'wsq_stress|notifier_protocol|lost_wakeup|dag_executor|exception_propagation|corun_deadlock|pipeline_ordering|dataflow_engine' \
    || { echo "FAILED on iteration $i"; break; }
done
```

**Result 2026-08-26: 300 iterations, all clean** (2,400 test executions on 8 real
cores). Nothing new fell out of the existing suite — the bug found on this
machine came from the *new* shallow-queue harness in §8a-bis, not from running
the old tests harder. Worth remembering when the next machine arrives: hammering
an existing test explores more interleavings of the same shape, and a bug that
needs a different shape will not appear however many times you run it.

Then under TSan (Apple clang supports it on arm64; no `setarch` needed):

```bash
CXX=clang++ cmake -S . -B build/tsan -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="-fsanitize=thread -fno-omit-frame-pointer -g"
cmake --build build/tsan -j
for i in $(seq 1 20); do
  TSAN_OPTIONS=halt_on_error=1 ctest --test-dir build/tsan --output-on-failure -LE measurement || break
done
```

**Remember what TSan cannot do**, because this repo already got caught by it: the
`park()` lost wakeup was invisible to TSan, since every access was under a mutex.
It was a protocol error, not a race. TSan-clean is necessary and nowhere near
sufficient — the repeated plain runs above matter just as much.

### 8c. Two free portability findings while you are there

**libc++ instead of libstdc++.** Every `sizeof` in this repo was measured against
libstdc++. Some of those numbers are in `NOTES.md` tables. If a `static_assert`
fires, that is a genuine finding, not a nuisance — record which and why. Known
differences to expect: `std::function` and `std::any` have different sizes, which
moves `sizeof(acpp::node)` (Module 11) and the Module 8 comparison table. Add the
libc++ numbers as a second column rather than replacing the libstdc++ ones.

**Cache-line size — measured, and not answerable here.**
`hardware_destructive_interference_size` is **256** against `ACPP_CACHELINE_SIZE`
of 64, so the `alignas` separating `top` from `bottom` may not be separating them
at all on Apple Silicon. The *layout* claim checks out (64/128/256 →
`sizeof(unbounded_wsq<int *>)` of 192/384/768, alignment to match). The
*throughput* claim does not resolve: at 8 threads the gap between the best of
each configuration is 1.37 ms while the `CL=64` row alone spans 17.12 ms.

**Read the methodology note in `modules/09-work-stealing-deque/NOTES.md` before
re-running this on the x86 box.** The first pass ran all repetitions of 64, then
128, then 256, and produced a clean "256 is 24% faster at 8 threads" that
round-robin interleaving made vanish entirely — on a fanless machine the
configuration that runs last is measured hottest, and the ordering *was* the
result. Interleave configurations, or do not compare them. **This still needs the
16-core box**, which can hold a clock steady.

### What NOT to do on the Air

Do not take throughput numbers there. It is fanless; a sustained benchmark
throttles, and the results will be worse than a 16-core WSL2 box for reasons that
have nothing to do with the code. Correctness runs are short and bursty and are
fine. Leave rows 1–7 of the table above to the x86 machine.

## Step 3 — where to write the results

**Do not overwrite the single-core numbers.** They are evidence about a machine,
and the contrast between the two is itself the point. Add a second table.

Use this shape, which matches how the existing notes are written:

```markdown
### Measured — 16-core WSL2 (2026-xx-xx)

`nproc` = 16, gcc 13.x, -O2, best of 5.

| ... | ... |

Against the single-core numbers above: <what changed, and whether the
prediction in the section above held>.
```

| Result | Goes in |
|---|---|
| 1 partitioners | `modules/12-parallel-algorithms/NOTES.md` → "Measured" under exercise 1 |
| 2 wsq scaling | `modules/09-work-stealing-deque/NOTES.md` → "Exercise 5" |
| 3 sticky victim | `modules/11-scheduler/NOTES.md` → "sticky victim, measured" |
| 4 dataflow | `modules/13-capstone/NOTES.md` → "Numbers"; mention the crossover in `docs/design.md` Decision 4 |
| 5 idle CPU | `modules/10-notifier/NOTES.md` → "Exercise 5" |
| 6 erasure table | `modules/08-type-erasure/NOTES.md` → "Exercise 1" |
| 7 cache misses | `modules/06-sparse-set/NOTES.md` → "Exercise 3 — measured" |
| ~~8a weakened fence~~ | **DONE** — `modules/09-work-stealing-deque/NOTES.md` → "Exercise 3" |
| ~~8b/8c ARM + libc++ findings~~ | **DONE** — Modules 8, 9 and 11 `NOTES.md`; libc++ added as a second column, libstdc++ numbers kept |
| any new bug | the owning module's `NOTES.md`, plus `docs/notes.md` under "Phase C" |

Then update, in this order:

1. **`README.md`** — the "What the measurements actually said" section. Several
   bullets there are hedged specifically because of the single core; if a
   prediction now holds or fails on real hardware, say so plainly.
2. **`docs/notes.md`** — the technique log. Entries citing a measurement carry
   the number; update those, and add an entry for anything new.
3. **`docs/CLAUDE.md`** — the toolchain table describes *this* machine. Either add
   a second machine or note which results came from where.
4. **This file** — strike out what is settled. If everything in Step 2 is done and
   nothing new is open, delete it and remove the README link.

## The standard to hold to

The repo's rule is in `docs/memory/measure-dont-assert.md`: a claim is checked by
the build, or it is stated as unverifiable with the reason. Two habits from that
which matter more with real cores:

- **Assert inside the benchmark that the work happened.** Two versions of the
  dataflow benchmark produced perfectly plausible numbers while measuring
  nothing — "best of N" was won by passes where the update was a no-op, and some
  graph inputs had no dependents. A benchmark that crashes gets fixed; one that
  quietly measures nothing gets quoted.
- **Report the spread, not just the best.** If best-to-worst on one row exceeds
  the gap between rows, the ranking is not established and the write-up should
  say exactly that.
- **Say which machine a number came from.** Every table gets a header line with
  the machine, core count, compiler and standard library. Two machines make this
  mandatory rather than polite.

## Two machines at once

They do not collide: the x86 box owns rows 1–7 and the M1 owns 8a–8c, and those
are different sections of different files. The repo commits directly to `main`
(no feature branches — `docs/memory/commit-directly-to-main.md`), so:

```bash
git pull --rebase        # before you start, and again before you push
```

Commit per finding rather than in one lump, so a rebase conflict is small and
obvious. If both machines somehow touch the same table, keep both rows — the
whole point is that the machine is part of the result.
