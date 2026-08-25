# Pending verification — re-run these on a multi-core machine

**Status:** every module and the capstone is implemented, tested and CI-green.
What is *not* settled is a set of **measurements**, because the machine they were
taken on cannot produce the conditions they describe.

**Read this first if you are the agent or person running on the 16-core box.**

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

If four cores found two bugs, sixteen may find more. That is the real reason for
this exercise; the numbers are the secondary benefit.

`perf stat` is also unusable here (`perf_event_paranoid = 4`), so cache work went
through cachegrind. WSL2 usually allows `perf`.

## Before anything else

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

### Still not answerable on WSL2

| # | What | Why |
|---|---|---|
| 8 | Module 9 exercise 3 — does a weakened memory order get caught? | x86-TSO hides it regardless of core count. Needs **ARM or POWER**, or a model checker (`herd7`, GenMC, CDSChecker). More cores raise the odds slightly; a pass still proves nothing. |
| 9 | Module 3 — MSVC `[[no_unique_address]]` layout | Needs MSVC. WSL2 does not have it, but the **Windows host might** — `cl.exe /std:c++20 /EHsc` on `modules/03-layout-economy/compressed_pair_layout.cpp` would settle it. |

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
