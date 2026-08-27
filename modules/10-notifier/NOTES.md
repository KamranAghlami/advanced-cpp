# Module 10 — Sleeping without lost wakeups

Source under study: `taskflow/core/nonblocking_notifier.hpp`, and
`Executor::_wait_for_task` / `_explore_task` in `taskflow/core/executor.hpp`.
Ancestor: Dmitry Vyukov's `EventCount` in Eigen's `ThreadPool`.
Reimplementation: `src/acpp/notifier.hpp`.

| Exercise | Deliverable |
|---|---|
| 1 — draw the `_state` word, compute the worker limit | below + `notifier_state_layout.cpp` |
| 2 — a *broken* pool, with the hang reproduced reliably | `lost_wakeup.cpp` |
| 3 — fix it with a 2PC notifier over mutex + condvar | `acpp::blocking_notifier` |
| 4 — replace the internals with a packed atomic word | `acpp::nonblocking_notifier` |
| 5 — idle CPU: spin vs 2PC vs condvar-per-push | `notifier_idle.cpp`, below |
| 6 — `sticky_victim` | Module 11 (`modules/11-scheduler`) — it needs an executor |

---

## Checkpoint: the lost-wakeup window, as a two-column timeline

```
   worker                                pusher
   ──────                                ──────
1  check every queue -> all empty
2                                        push(task)
3                                        notify()  ── nobody is registered,
4                                                     so this goes nowhere
5  park()  ─────────────────────────────────────────► sleeps forever,
                                                       with work pending
```

Lines 1 and 5 are separated by an arbitrary amount of time — a scheduler
preemption, a cache miss, a page fault. The pusher's entire operation fits in
that gap.

**The instruction that closes it** is the `fetch_add` in `prepare_wait`, made
visible by the `seq_cst` fence immediately after it:

```cpp
void prepare_wait(size_t id) {
    waiters[id].epoch = state.fetch_add(prewaiter_inc, memory_order_relaxed);
    atomic_thread_fence(memory_order_seq_cst);   // <-- this
}
```

The timeline becomes:

```
   worker                                pusher
   ──────                                ──────
1  prepare_wait()  -> pre-waiter count is now 1, PUBLISHED
2  re-check queues -> still empty
3                                        push(task)
4                                        notify() ── sees prewaiters == 1,
5                                                    consumes the slot and
6                                                    bumps the epoch
7  commit_wait() -> epoch already moved past our ticket -> DO NOT PARK
```

The window is closed by **ordering**, not by a lock. The announcement is
published before the re-check, so any notify after the re-check still finds
somebody to tell.

Without the fence, the `fetch_add` may sit in a store buffer while the
predicate load at line 2 has already retired — the announcement is not yet
visible to the pusher, and the whole protocol buys nothing. Same store-load
problem as Module 9's `pop`, same instruction to fix it.

**The rule**: `prepare_wait` must be followed by exactly one of `commit_wait` or
`cancel_wait`, with the same id. Violating it is UB, and `notifier_protocol.cpp`
traces all four exits from the window — work appeared, shutting down, nothing to
do (park), woken — with exactly one resolution on each.

## Exercise 1 — the state word

```
 63                                  32 31              16 15               0
┌──────────────────────────────────────┬──────────────────┬──────────────────┐
│               epoch                  │    prewaiters    │      stack       │
│              32 bits                 │     16 bits      │     16 bits      │
└──────────────────────────────────────┴──────────────────┴──────────────────┘
  bumped once per fulfilled request      threads between    index of the top
  ABA defence + ticket ordering          prepare and        parked waiter, or
                                         commit             0xFFFF = empty
```

| field | mask | purpose |
|---|---|---|
| `stack` | `0x000000000000FFFF` | head of an intrusive lock-free stack of parked waiters. All-ones is the empty sentinel. |
| `prewaiters` | `0x00000000FFFF0000` | how many threads are mid-protocol. Must be able to count every worker at once. |
| `epoch` | `0xFFFFFFFF00000000` | bumped by every CAS that resolves a request. |

**Maximum workers: 65,534.** Sixteen stack bits give 65,536 values; one is spent
on the empty sentinel (65,535 indices remain), and the constructor refuses
anything that would let a waiter index alias it. `notifier_state_layout.cpp`
checks that the fields tile the word exactly with no gaps and no overlap, and
that oversizing **throws** rather than aliasing — the failure it prevents is a
hang, not a crash, and a hang has no stack trace.

**Why the increments are one unit in the right field.** `prewaiter_inc` and
`epoch_inc` are single bits at the field shifts, so
`state - prewaiter_inc + epoch_inc` leaves the pre-wait stage *and* bumps the
epoch in one CAS. Two field updates, one atomic operation, no intermediate state
anyone can observe.

**The epoch is two things at once**, which is the part worth slowing down for:

- an **ABA defence** — a stale read cannot win a CAS even if the stack index
  happens to match, because the epoch has moved;
- a **ticket** — a pre-waiter records the state it saw, and
  `epoch - recorded_ticket` tells it whether its turn has arrived (`== 0`),
  already passed (`> 0`, meaning it was notified), or not yet come (`< 0`, so
  yield and re-read). That is what serialises resolutions in arrival order
  without a lock.

**The epoch wraps** — it is 32 bits. Harmless, because every comparison is a
*signed* difference of two epochs that are close together, and unsigned
subtraction reinterpreted as signed gives the true difference as long as the
real gap fits the signed range. The gap is bounded by the number of workers, so
it does. Checked in `notifier_state_layout.cpp`.

**One place that deliberately does *not* bump the epoch**: popping a waiter off
the stack in `notify_one`. ABA on the stack cannot happen, because a waiter is
only ever re-pushed after passing through the pre-wait state, and that always
bumps the epoch. Worth noting because it looks like an omission.

## Exercise 2 — the bug, made deterministic

`lost_wakeup.cpp` does not wait for the race; it *constructs* it. The worker
checks its predicate, then blocks on a handshake until the pusher has both
pushed and notified, and only then sleeps.

Measured: **the worker sleeps 50.8 ms with work already queued**, and it only
wakes at all because the naive `wait_for` has a timeout. Without that bound the
join never returns — which is what the bug looks like in production: a pool that
stops making progress and cannot say why.

That is the skill the exercise is really about. A race that reproduces every run
in a few milliseconds is a bug you can fix; the same race at one-in-a-week is a
mystery.

The same interleaving against the two-phase protocol returns in **0.0 ms**, on
both implementations.

## Exercises 3 and 4 — two implementations, one interface

`blocking_notifier` first, deliberately: get the *protocol* right before getting
it lock-free. Everything under one mutex, so the invariant can be stated rather
than argued — a signal is either delivered to a parked waiter, held for a
pre-waiter about to commit, or dropped because nobody is in the protocol at all,
and the third is safe because a thread outside the protocol has not yet
re-checked its predicate.

`nonblocking_notifier` then replaces the internals with the packed word and the
intrusive stack. Same interface, so `notifier_protocol.cpp` and
`notifier_idle.cpp` are written once and instantiated twice.

Both are TSan-clean under the protocol stress test: 20,000 items, 4 workers,
bursty submission, every item consumed exactly once, several thousand real
parks. The `parks > 0` assertion matters — without it a run where nobody ever
slept would pass and prove nothing.

## The lost wakeup inside the lost-wakeup fix

`nonblocking_notifier` passed every local test, was TSan-clean, and **hung CI's
four-core runner** — `notifier_protocol` timed out at 300 s where it takes 0.3 s
here.

The bug was in `park()`:

```cpp
static void park(waiter *self) {
    std::unique_lock guard{self->mutex};
    self->state = waiting;                                   // <-- erases a signal
    self->cv.wait(guard, [self] { return self->state == signaled; });
}
```

`commit_wait` publishes the waiter on the stack with a CAS, then calls `park()`.
A notifier can pop that waiter and set `signaled` **in the window between the
CAS and this thread acquiring the mutex**. Writing `waiting` on entry then erases
the signal, and the wait never ends.

It is the module's own bug, one level down: the two-phase commit closes the
window between "I checked" and "I registered", and this reopened a window
between "I registered" and "I slept".

Two things worth taking from it:

- **The fix is to not write state on the way in.** Clear `not_signaled` under
  the mutex *before* publishing, and let `park` only read. State a waiter
  publishes must not be re-initialised after it becomes reachable.
- **TSan cannot see this.** Every access is under the waiter's mutex, so there
  is no data race — it is a *protocol* error. Phase C's ground rule is that
  concurrent code is TSan-clean before it is believed, and this is the reminder
  that TSan-clean is necessary and nowhere near sufficient.

And, again: a single-core machine cannot validate a concurrent design. The
window here is a handful of instructions wide, and one core essentially never
lands inside it.

## Exercise 5 — idle CPU, and a result the course does not predict

Bursty load: 12 bursts of 40 items with 25 ms of idle between them, 4 workers.
`getrusage` CPU seconds, not wall time — wall time is dominated by the idle gaps
and says almost nothing.

| strategy | CPU (s) | wall (ms) | parks |
|---|---:|---:|---:|
| spin (yield loop) | **0.2590** | 301.4 | 0 |
| 2PC `blocking_notifier` | 0.0054 | 308.0 | 372 |
| 2PC `nonblocking_notifier` | 0.0033 | 305.5 | 346 |
| condvar, signal on every push | **0.0015** | 303.8 | 480 |

**Spinning costs ~79× the CPU of any sleeping strategy** for identical wall-clock
throughput. That is the number the course is pointing at, and it is not subtle:
on a battery-powered target it is the difference between a device that idles and
one that does not.

**But condvar-per-push has the *lowest* idle CPU here**, and pretending otherwise
would be the dishonest version of this module. The reason is that this harness
gives all three strategies the same mutex-guarded `std::queue`, so the condvar
version's lock on the push path is one it was paying for anyway. Its `notify_one`
is a bare futex wake; the 2PC version additionally pays a `fetch_add` and a CAS
per drain attempt.

So the honest conclusion is:

- against **spinning**, the two-phase notifier wins overwhelmingly on idle power;
- against **condvar-per-push**, it does *not* win on idle CPU, and the advantage
  it is designed for — **no synchronisation on push when nobody is sleeping** —
  is invisible in this measurement because the queue lock dominates.

Showing that advantage requires the push path to be lock-free, which means
Module 9's work-stealing queue, which means Module 11's executor. Noted there
rather than claimed here.

### Measured — 16-core WSL2 (2026-08-27)

Same harness, same shared mutex-guarded queue, `nproc` = 16, gcc 13.3 `-O2`,
Debug build, 4 workers, 3 runs (best per column):

| strategy | CPU (s) | wall (ms) | parks (range across 3 runs) |
|---|---:|---:|---:|
| spin (yield loop) | **1.1182** | 301.8 | 0 |
| 2PC `blocking_notifier` | **0.0024** | 303.2 | 63–89 |
| 2PC `nonblocking_notifier` | 0.0048 | 304.6 | 239–364 |
| condvar, signal on every push | 0.0096 | 310.9 | 480 (every run) |

Against the single-core numbers, this is a **reversal, not just a bigger
gap**: on one core condvar had the lowest idle CPU (0.0015s, beating 2PC's
0.0033–0.0054s); on 16 real cores **2PC `blocking_notifier` is lowest**
(0.0024s vs condvar's 0.0096s) — 4× better, in the direction the design
argument always predicted, on a benchmark whose push path is *still* mutex
Guarded exactly as before. Nothing about the harness changed; only the
hardware did — the push path is still mutex-guarded, exactly as before.

The `parks` column is the mechanism, and it moved the most: `blocking_notifier`
parks on only 63–89 of 480 pushes here, down from 372 on one core — the 2PC
guard's "don't park if a waiter would just find work already there" check is
catching far more cases with real parallelism than with time-slicing, because
a genuinely-concurrent burst is more likely to have a worker already spinning
past the check when the item lands. `condvar` still signals on literally every
push (480 parks, both machines, by construction) and pays the mutex + futex
wake cost every time regardless of whether anyone needed waking — that fixed
cost is what 16 real cores exposed and 1 time-sliced core hid. This still is
not the *lock-free-push* advantage the design argument is ultimately about
(the queue lock is still shared by all three strategies here); it is the 2PC
guard's redundant-wakeup avoidance winning even before the push path itself
is decontended.

## What `_wait_for_task` adds on top

The notifier is the mechanism; the executor's wait loop is where it meets a real
scheduler. Reading `Executor::_wait_for_task`, the parts worth carrying into
Module 11:

- **Every exit path calls exactly one of cancel/commit.** All six. That
  discipline is why the source uses `goto` — it is genuinely clearer than nested
  breaks, and the source says so.
- **`_num_topologies == 0` is a relaxed load used as a hint.** The justification
  is not "it is faster" but "a missed update is caught by the 2PC guard anyway".
  Same shape of argument as Module 9's `_cached_top`.
- **`sticky_victim`**: after a successful steal, remember who you stole from and
  try them first next time. Producer/consumer relationships in a task graph are
  stable, so this converts random stealing into near-directed stealing. Exercise
  6, done in Module 11.
- **Bounded steal attempts**: `MAX_STEALS`, then `yield()`, then a hard cap.
  Never spin forever.
- **Sampling a victim that is not yourself**, without a rejection loop:

  ```cpp
  vtm = rdgen() % (MAX_VICTIM - 1);
  if(vtm >= id) vtm++;
  ```

  A draw over `[0, N-1)` mapped onto `[0, N) \ {id}` with one modulo and one
  predicated increment. Rejection sampling ("draw again if you got yourself")
  has unbounded worst-case latency; this has none. Requires `N >= 2`, which the
  constructor guarantees. (`%` over a non-power-of-two range has modulo bias, so
  "uniform" is approximate — negligible here, but worth knowing you are waving
  it away.)

**The trap the course warns about is real.** `executor.hpp` contains two
definitions of `_explore_task`, one live and one commented out immediately below
it, and they differ in exactly the way that matters — the dead one lacks the
self-exclusion above and can pick itself as a victim:

```
$ grep -n "_explore_task" third_party/taskflow/taskflow/core/executor.hpp | wc -l
```

Grep before reading, and check the hit count against what you expected.

## Techniques logged

Added to `docs/notes.md`: two-phase commit wait, the packed atomic state word
with an epoch serving as both ABA defence and ticket, and the idle-power result.
