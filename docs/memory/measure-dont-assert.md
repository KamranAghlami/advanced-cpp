---
name: measure-dont-assert
description: Claims in this repo must be checked by the build, not asserted in prose — and stated honestly when the machine cannot check them.
metadata:
  type: feedback
---

Every performance or codegen claim in this repo has to be verified by something
the build re-runs: `cmake/check_asm.cmake` for codegen, `check_compile_fails.cmake`
for "this is ill-formed", `check_symbols.cmake` for emission, `check_manifest.cmake`
for the dependency surface. Prose claims rot; checked ones do not.

**Why:** the course text repeatedly says "verify this yourself" and the whole
point of the repo is not taking folklore on trust. Several course predictions
turned out to be wrong when measured (`std::function` inlining, `std::visit`
codegen, the partitioner table).

**How to apply:** when a measurement cannot be made on this machine — one shared
vCPU, `perf_event_paranoid=4`, no multi-core scaling — say so next to the number
and report `nproc`, rather than quoting a number that means nothing. A
measurement that quietly measures nothing is worse than none, because it gets
quoted. Assert inside benchmarks that the work actually happened. See
[[comment-style]].
