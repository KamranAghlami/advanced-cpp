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

> **Update 2026-08-27 — the 16-core x86 WSL2 run happened.** Rows **1–7 are
> now all measured** — see the table below each row for what changed and what
> didn't (headline: Module 12's "each partitioner wins one" still doesn't
> hold, now for a *tested* reason; Module 10's 2PC notifier reverses and beats
> condvar on real cores; Module 9 gets its real 1→16-thread scaling curve).
> `perf` is confirmed unusable here too, for a third, unrelated reason (WSL2's
> kernel has no matching `linux-tools` package) — cachegrind stays the only
> working instrument in this repo.
>
> **The important result wasn't on the list.** §8a's "SETTLED... no, and it
> never could have been" turns out to describe only the ARM half. Running
> `wsq_weakened` (Module 9's deliberately fence-weakened build) on 16 real x86
> cores under clang: **199 failures in 200 runs, control 0/200.** The fence
> weakening was never vacuous on x86 — it was only ever run on a single core,
> which cannot open the window, or (misleadingly) assumed to generalize from
> the ARM result. A second, unplanned finding fell out while confirming this
> at the instruction level: **gcc's x86-64 codegen is a second vacuous
> pairing** — it emits the full `seq_cst` barrier for `acq_rel` too, so
> `wsq_weakened` stays clean under gcc on the same 16 cores (0/50) for the
> same structural reason ARM is clean under clang. See §8d below — this is
> the most load-bearing finding in the document. `wsq_weakened` was an
> unfiltered blocking CI test; it is now `acpp_exercise` (build-only, not
> `ctest`-registered), matching its sibling `wsq_weakened_release`.

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

**Result on the M1 (2026-08-26): all six legs green, 47 tests each** — ASan/UBSan
with no diagnostics, TSan with no race reports. ~14 minutes.

Two caveats on that run, both worth knowing before trusting a macOS `verify.sh`:

- **The `gcc-20` and `gcc-23` legs are not gcc.** `g++` on macOS is a shim for
  Apple clang, and no Homebrew gcc was installed, so those legs re-test clang at
  two standards. Cross-compiler coverage still needs Linux (or `brew install
  gcc`), and a green macOS matrix is four clang legs plus two sanitizers.
- **Do not interrupt it.** Killing a leg mid-clone leaves `third_party/<dep>`
  without a valid `.git`, and the *next* leg fails at configure with
  `Failed to get the hash for HEAD`. It self-heals on the following leg, so the
  symptom is one spuriously failed leg in the middle of a green run. That is the
  shared-`third_party` hazard below, reached by interrupting rather than by
  running two builds at once.

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

### High value — **all four DONE 2026-08-27, 16-core WSL2**

| # | Run | Single-core result | Result on 16 real cores |
|---|---|---|---|
| ~~1~~ | `partitioner_bench` | spread ~10%, mostly noise | **Prediction still doesn't hold, now for a tested reason.** Guided wins uniform, proportional, *and* heavy-tail (not one each) — real this time, spread ≤0.15 ms vs multi-ms gaps between workloads. Detail in `modules/12-parallel-algorithms/NOTES.md`. |
| ~~2~~ | `wsq_bench` | 1.14× uncontended, "threads" column was oversubscription | **Real scaling curve obtained** (sweep extended to 16 threads in `wsq_bench.cpp`): parity at 1 thread → 20.4× at 16. Mutex serialises regardless of core count (30 ms → 557 ms); Chase–Lev's contention cost rises far more slowly (5 ms → 27 ms). `modules/09-work-stealing-deque/NOTES.md`, Exercise 5. |
| ~~3~~ | `sticky_victim_bench` | 23% fewer steal attempts, 14.5%→19.8% hit rate | **Effect survives but is noisier than implied** — attempts ratio ranges 0.633–1.004 across 5 runs (one was a wash). Success rate is never worse than random's, though, in all 5. `modules/11-scheduler/NOTES.md`. |
| ~~4~~ | `dataflow_bench` | `incr-par` **lost** to `incr-ser` at every size | **Crossover found, bracketed between 64 and 295 recomputed cells.** `incr-par` wins at 295 (1.51×) and 867 (1.80×) recomputed cells, still loses at 64. `modules/13-capstone/NOTES.md`. |

### Medium value — **all three DONE 2026-08-27, 16-core WSL2**

| # | Run | Single-core result | Result on 16 real cores |
|---|---|---|---|
| ~~5~~ | `notifier_idle` | spin 0.259 s CPU; 2PC 0.0033 s; **condvar 0.0015 s** | **Reverses.** 2PC `blocking_notifier` drops to 0.0024 s vs condvar's 0.0096 s — 4× better, **without** the push path becoming lock-free. Mechanism: the 2PC guard parks on only 63–89/480 pushes here (vs 372 on one core) — real concurrency means a waiter is more often already past the check. `modules/10-notifier/NOTES.md`, Exercise 5. |
| ~~6~~ | `erasure_table` | ranking **not established** — spread within a row exceeded differences between rows | **Resolves into three tiers** on a quiet 16-core box: raw pointer ≈ virtual call < delegates < `std::function`/`poly`. Per-row noise dropped to ≤0.05 ns (was 3–10 ns), an order of magnitude below the gaps between tiers. `erasure_table.cpp`'s hardcoded "1 vCPU" machine label is now `hardware_concurrency()`-driven. `modules/08-type-erasure/NOTES.md`. |
| ~~7~~ | `sparse_set_bench` | 5.6× over `unordered_map` at 1M; cachegrind for misses | **`perf` still unusable — third distinct reason.** WSL2's Microsoft-patched kernel has no matching `linux-tools` package in the Ubuntu archive (`perf_event_paranoid` is 2 here, better than the droplet's 4, and irrelevant). Wall-clock re-measured instead: the two ratios move opposite ways on a faster core — iteration's edge shrinks (5.3–5.6× → 2.7–3.7×), lookup's edge grows (5.6× → 7.3×). `modules/06-sparse-set/NOTES.md`. |

### Row 9 — still open; row 8 fully closed as of 2026-08-27

| # | What | Why |
|---|---|---|
| ~~8~~ | ~~Module 9 exercise 3 — does a weakened memory order get caught?~~ | **DONE 2026-08-26/27, both knobs, both platforms.** ARM catches the *release* weakening (156/200), x86+clang catches the *fence* weakening (199/200, 2026-08-27) — and gcc's x86 codegen turns out to be a second vacuous pairing next to clang's ARM one. See §8d — this one has a CI-flakiness implication that is not yet resolved. |
| 9 | Module 3 — MSVC `[[no_unique_address]]` layout | **Settled a different way** — CI's `msvc` leg (blocking since 2026-08-26) answers this on every push; see `modules/03-layout-economy/NOTES.md`. Still true that `cl.exe` on a Windows host would answer it locally, but no longer necessary. |

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
16-core box, which can hold a clock steady — not yet done as of 2026-08-27.**
No existing target builds the three `ACPP_CACHELINE_SIZE` configurations; it
needs its own harness (three cmake configures or a compile-time-parameterised
build, interleaved per the note above), not a rerun of an existing binary.

### 8d. The fence half found its machine — 16-core WSL2, clang (2026-08-27)

§8a concluded "no, and it never could have been" from the M1 alone — true for
ARM, and wrongly generalized to x86 by omission. Nobody had run
`wsq_weakened` on a machine with both **x86** (where clang's codegen actually
removes the fence, per the table in §8a) **and real parallelism** (which
neither the droplet nor the M1's vacuous-on-ARM result could supply) until
now:

```bash
for i in $(seq 1 200); do
  ctest --test-dir build --output-on-failure -R '^wsq_weakened$' || echo "run $i failed"
done
# result: 199/200 failed. Control (wsq_stress, same 200 runs): 0/200 failed.
```

**This is a real, reproducible bug catch, not a probabilistic curiosity** —
first failure typically appears within the first few runs. Confirmed at the
instruction level, not just from the pass rate: `objdump` on `steal()` shows
the two builds differ in exactly one place, the runtime `memory_order` value
passed into the fence call (`$0x5` seq_cst vs `$0x4` acq_rel); under clang,
`acq_rel` takes a codepath that emits no hardware instruction, matching the
`<nothing at all>` cell in §8a's table.

**A second, unplanned finding while confirming that: gcc's x86-64 codegen
does not distinguish the two orders either.** `objdump` on the *gcc*-built
`wsq_weakened` shows the identical `lock orq $0x0,(%rsp)` at the fence site
in both the weakened and control builds. 50 runs of `wsq_weakened` under gcc
on the same 16-core box: **0 failures.** GCC's x86 fence codegen is
conservative in exactly the way that makes the weakening experiment vacuous
— a second such pairing next to clang/ARM, on a different axis (compiler,
not ISA). The corrected 2×2:

| | clang | gcc |
|---|---|---|
| **x86, real cores** | catches it — 199/200 | vacuous — 0/50, barrier unconditional |
| **ARM (M1), real cores** | vacuous — 0/500, both orders → `dmb ish` | not measured |

**This had an operational implication that went beyond documentation, and it
is fixed as of 2026-08-27.** `wsq_weakened` was registered as a plain
`add_test`, running unfiltered inside CI's blocking `build` job
(`.github/workflows/ci.yml`), on both the gcc and clang legs, on GitHub-hosted
`ubuntu-24.04` runners. Those runners are not 16-core, but they are
multi-core, and the failure rate needed to make a CI job flaky is far below
99.5%.

`wsq_weakened` is now `acpp_exercise`, not `acpp_test`
(`modules/09-work-stealing-deque/CMakeLists.txt`) — it still builds, and is
still runnable by hand, but is no longer registered with `ctest` at all, so
it cannot gate CI. This is not a new pattern: it makes `wsq_weakened` match
`wsq_weakened_release`, which was *already* `acpp_exercise` for exactly this
stated reason ("a test whose expected result depends on the machine is not a
regression gate") — the inconsistency was that `wsq_weakened` hadn't been
updated to match once its own expected result turned out to be
machine-dependent too.

**Whether the clang leg of `build` was already intermittently red or
silently re-run-until-green before this fix landed is still unknown** — that
needs CI run history, which this session could not check (`gh` was
unauthenticated here, and the repository was not reachable over the plain
GitHub API either). Worth a look if anyone has console access to Actions
history for this repo.

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
| ~~1 partitioners~~ | **DONE** — `modules/12-parallel-algorithms/NOTES.md` → "Measured — 16-core WSL2" under exercise 1 |
| ~~2 wsq scaling~~ | **DONE** — `modules/09-work-stealing-deque/NOTES.md` → "Exercise 5" |
| ~~3 sticky victim~~ | **DONE** — `modules/11-scheduler/NOTES.md` → "Measured — 16-core WSL2" |
| ~~4 dataflow~~ | **DONE** — `modules/13-capstone/NOTES.md` → "Numbers"; crossover mentioned in `docs/design.md` Decision 4 |
| ~~5 idle CPU~~ | **DONE** — `modules/10-notifier/NOTES.md` → "Exercise 5" |
| ~~6 erasure table~~ | **DONE** — `modules/08-type-erasure/NOTES.md` → "Exercise 1" |
| ~~7 cache misses~~ | **DONE, but `perf` stayed unreachable** — `modules/06-sparse-set/NOTES.md` → "Note on tooling"; wall-clock re-measured instead |
| ~~8a weakened fence~~ | **DONE** — `modules/09-work-stealing-deque/NOTES.md` → "Exercise 3" |
| ~~8b/8c ARM + libc++ findings~~ | **DONE** — Modules 8, 9 and 11 `NOTES.md`; libc++ added as a second column, libstdc++ numbers kept |
| ~~8d x86 fence, gcc's vacuous pairing~~ | **DONE** — `modules/09-work-stealing-deque/NOTES.md` → "The fence half found its machine too" and "Checkpoint"; `src/acpp/wsq.hpp` comment updated. **CI-wiring decision still open — see §8d.** |
| cache-line-size throughput (§8c) | **still open** — needs its own harness, not a rerun; not attempted 2026-08-27 |
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
