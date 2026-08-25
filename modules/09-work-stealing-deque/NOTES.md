# Module 9 — The Chase–Lev work-stealing deque

Source under study: `taskflow/core/wsq.hpp` (`UnboundedWSQ`, `BoundedWSQ`).
Paper: Lê, Pop, Cohen, Zappa Nardelli, *Correct and Efficient Work-Stealing for
Weak Memory Models*, PPoPP'13.
Reimplementation: `src/acpp/wsq.hpp`.

| Exercise | Deliverable |
|---|---|
| 1 — answer the four §9.2 questions **in writing** | below |
| 2 — the bounded version from scratch | `acpp::bounded_wsq` |
| 3 — owner + N thieves, exactly-once, under TSan; then weaken an order | `wsq_stress.cpp`, `wsq_weakened` |
| 4 — the unbounded version, growth and garbage, instrumented | `acpp::unbounded_wsq`, `wsq_semantics.cpp` |
| 5 — against `std::deque` + `std::mutex` | `wsq_bench.cpp` |

---

## Exercise 1 — the four questions

### 1. Why is the owner's `_bottom` load `relaxed` when a thief's is `acquire`?

Because they are asking different questions.

The owner is the **only writer** of `bottom`. Reading a variable you alone write
needs no ordering at all: the program order of a single thread already guarantees
you see your own last store. `relaxed` here is not an optimisation gamble, it is
the accurate statement that no inter-thread ordering is required.

A thief reads `bottom` to decide whether a slot is safe to read, and the slot was
written by the owner *before* the owner's `release` store to `bottom` in `push`.
The thief's `acquire` is the other half of that release/acquire pair: it is what
makes the owner's write to `buffer[b]` visible to this thief. Drop it to
`relaxed` and the thief may read a slot whose contents have not been published —
an actual data race on the payload, not a lost task.

So: the owner's `relaxed` is about *self*-ordering (there is none to establish);
the thief's `acquire` is about *data* ordering (there is, and it is the payload).

### 2. What exactly does the `seq_cst` fence prevent?

It prevents the store to `bottom` from being reordered **after** the load of
`top`.

```cpp
bottom.store(b, relaxed);                 // (A) "I am claiming the bottom slot"
atomic_thread_fence(seq_cst);
auto t = top.load(relaxed);               // (B) "where is the top?"
```

Store-then-load to *different* locations is the one reordering that every real
CPU performs and that no acquire or release order forbids. Store buffering: (A)
sits in this core's store buffer while (B) is satisfied from cache.

**The interleaving that breaks without it.** One element in the queue,
`top == bottom == 5`:

| | owner | thief |
|---|---|---|
| 1 | `b = 4`; store `bottom = 4` **buffered, not visible** | |
| 2 | | loads `top = 5` |
| 3 | | fence |
| 4 | | loads `bottom = **5**` (the old value) |
| 5 | | `5 < 5` is false → sees an empty queue... |
| 6 | loads `top = 5` | |
| 7 | `t(5) <= b(4)` is false → owner sees an empty queue too | |

Both conclude the queue is empty and the element is **lost**. The mirror
interleaving — where the store lands late and the thief still reads the old
`bottom` while the owner reads the old `top` — has both of them take the *same*
element, which is worse: the task runs twice.

The fence turns (A) and (B) into a pair the whole system agrees on the order of.
It has to be `seq_cst` on *both* sides: `acq_rel` on a fence orders
load-load, load-store and store-store, but **not** store-load. That is precisely
the one ordering needed here, which is why weakening it (exercise 3) is exactly
the wrong weakening and why nothing less than `seq_cst` will do.

Note what the fence is *not* doing: it is not protecting the payload. That is the
release/acquire pair on `bottom` from question 1. Two different jobs, two
different mechanisms, in the same function.

### 3. Why `compare_exchange_strong` and not `weak`?

`weak` is allowed to fail spuriously, and it is faster than `strong` only on
architectures where `strong` has to add a retry loop around LL/SC — ARM, POWER.
The rule of thumb is: use `weak` when you are already in a loop, `strong` when
you are not.

Neither CAS here is in a loop. Both are a **single, final** arbitration of "who
gets the last element", and the answer to a spurious failure would be to try
again — which means wrapping it in the loop `strong` already contains, only
worse, because the retry would have to re-read `top` and re-derive the decision.

There is also a correctness-shaped reason on the owner's side. The owner's CAS is
not really trying to acquire anything; it is asking "did a thief already take
this?" A spurious failure would answer "yes" when the truth is "no", and the
owner would drop an element it legitimately owned. The element is not lost — the
next `pop` finds it, because `bottom` is restored — but the owner would have
returned "empty" from a non-empty queue, which callers above it treat as a signal
to go stealing or to sleep. Correct, and needlessly slow, for no benefit.

### 4. Why does the owner CAS `top` at all, when it operates on the *bottom*?

Because when exactly one element remains, `top` and `bottom` name the **same
slot**, and the owner and a thief are reaching for the same object. At that point
"bottom" and "top" are not two ends of anything.

`bottom` cannot be the arbiter: only the owner writes it, so a thief has no way
to lose a race on it. `top` is the only variable both sides write, so it is the
only place the two can meet. The owner therefore steps onto the thieves' turf for
exactly one operation — the CAS that says "I am taking element `t`" — and the
CAS's atomicity decides it.

The asymmetry after that is worth noticing: the owner CASes `top` **and then
restores `bottom`** whatever the outcome, so the queue ends up empty and
consistent either way. A losing thief simply returns the sentinel. The owner does
more bookkeeping because the owner is the only one who can.

## The `_cached_top` trick

```cpp
if(array->capacity < (b - cached_top + 1)) {
    cached_top = top.load(acquire);          // only now pay for the real one
    if(array->capacity < (b - cached_top + 1)) { grow(); }
}
```

`top` only ever **increases**. So a stale `cached_top` is always ≤ the real
`top`, which makes `b - cached_top` always ≥ the real occupancy. The cached value
can therefore only make the queue look *fuller* than it is, and the consequence
of being wrong is one unnecessary load of the real `top` — never a missed resize.
The variable is owner-private and non-atomic, so it lives off the contended cache
lines entirely.

This is the shape of argument to imitate: not "relaxed is faster" but "here is
why a stale value is harmless, in the direction it can be stale".

## Cache-line discipline

`top`, `bottom` and the buffer pointer each get their own `alignas(64)`. The
owner writes `bottom` on every push and pop; thieves write `top` on every
successful steal. Sharing a line would mean every steal attempt invalidates the
owner's line and vice versa — false sharing on the two hottest words in the
scheduler.

## Exercise 4 — growth and the retained garbage

Old buffers are **never freed** while the queue lives, because a thief may still
hold a pointer into one it read before a resize. The alternative is hazard
pointers or epoch-based reclamation; the retained-garbage choice is acceptable
here only because of the arithmetic: each resize *doubles*, so the total retained
is bounded by the final capacity, and the number of resizes is log₂ of it.

Measured (`wsq_semantics.cpp`, `wsq_stress.cpp`):

| run | start | final capacity | resizes | retained buffers |
|---|---|---|---|---|
| 64 pushes from 4 slots | 4 | 64 | 4 | 4 |
| 200,000-item stress, 3 thieves | 256 | 16,384 | 6 | 6 |

Six resizes for 200,000 items is the answer to "how many resizes actually
happen in your workload": the queue's *depth* is what grows, not its throughput,
and the depth is bounded by how far the owner runs ahead of the thieves. On the
TSan run, where thieves get scheduled more aggressively, it was three.

## Exercise 3 — weakening a memory order on purpose

`wsq_weakened` is `wsq_stress.cpp` compiled with `-DACPP_WSQ_WEAKEN_FENCE`, which
turns both `seq_cst` fences into `acq_rel` — the one weakening that removes the
store-load ordering the algorithm depends on.

**It passes.** 200,000 items, three thieves, every item consumed exactly once.

That is the expected result and the whole point of the exercise. x86-TSO
guarantees store-load ordering in hardware for the store-buffer case *only* via
the fence's `mfence`; what actually saves the weakened build is that the
compiler still emits enough ordering, and that a single-core machine cannot
produce the interleaving at all. **On this machine the test cannot fail**, and
the honest conclusion is:

- a passing stress test on x86 is **not evidence** that a memory order is
  correct — it is evidence that you did not hit the window;
- the argument in question 2 above is the evidence; the test only guards against
  regressions in things the machine *can* exhibit;
- confirming it needs ARM or POWER hardware, or a model checker
  (CDSChecker / GenMC / `herd7` against the C11 model), none of which is
  available here.

Flagged rather than glossed. The weakened target stays in the build as a
recorded expectation, with the comment saying it is expected to pass.

## Exercise 5 — against a mutex-guarded deque

400,000 items, gcc `-O2`, **one core**:

| threads | Chase–Lev (ms) | mutex + deque (ms) | ratio |
|---:|---:|---:|---:|
| 1 | 26.53 | 30.29 | 1.14× |
| 2 | 10.89 | 26.12 | 2.40× |
| 4 | 9.44 | 29.64 | 3.14× |
| 8 | 12.99 | 30.80 | 2.37× |

**These are not scaling numbers and must not be read as any.** `nproc` is 1, so
the "threads" column is oversubscription: N threads time-slicing one core.
`docs/CLAUDE.md` says to report `nproc` next to anything concurrent, and this is
why.

What the table does support:

- **Uncontended (1 thread), the lock-free version is only 1.14× ahead.** An
  uncontended `std::mutex` on Linux is a futex fast path — a couple of atomics —
  so the gap over an atomic protocol is small. Anyone expecting an order of
  magnitude from "lock-free" on the uncontended path is expecting the wrong
  thing.
- **The mutex version is flat at ~30 ms regardless of thread count**, because
  every operation serialises through one lock whether or not there is a core to
  run on. The lock-free version's time *drops* with thieves.

That drop is not parallel speedup and deserves the honest explanation: with no
thieves the owner pushes all 400,000 items before draining them, so the queue
grows to hold the lot — more resizes, a much larger working set, worse locality.
With thieves the queue stays shallow. The lock-free queue is winning here on
*memory behaviour*, not on parallelism, and a benchmark that let that pass as
"scales better" would be lying.

The real scaling curve the exercise asks for needs a multi-core machine.

## Checkpoint

The four answers above are the checkpoint, and the test that would fail on a weak
memory machine exists (`wsq_weakened`) — with the caveat, stated above, that this
machine cannot run it meaningfully.

## Techniques logged

Added to `docs/notes.md`: the Chase–Lev protocol, the store-load fence, cache-line
isolation of contended atomics, monotonic-bound caching, and retained-garbage
reclamation.
