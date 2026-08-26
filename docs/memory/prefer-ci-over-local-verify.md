---
name: prefer-ci-over-local-verify
description: Push to CI rather than running scripts/verify.sh when CI is the faster or only way to get the answer.
metadata:
  type: feedback
---

Kamran would rather commit and push and read the GitHub runner's results than
wait on `scripts/verify.sh` locally. Asked directly: "isn't it faster to commit
and get results off of the github runner? if yes, commit and push, then wait for
the results."

**Why:** `verify.sh` runs six configure/build/ctest cycles *sequentially* on a
1-vCPU droplet; CI runs eight jobs in parallel on multi-core runners. And since
2026-08-26 CI includes a blocking `msvc` leg that no local script can reproduce
at all — a third ABI, and the one that actually catches layout mistakes. A green
`verify.sh` means "survives two Unix toolchains", not "portable".

**How to apply:** default to push-and-watch, especially for portability,
layout, or anything MSVC-sensitive. Reach for `verify.sh` when the change is
risky enough that landing a break on `main` is the real cost — this repo commits
straight to main with no branches ([[commit-directly-to-main]]) — or when
iterating faster than CI turnaround. Don't run it "to be thorough" when CI
answers the same question sooner. Use a Monitor on `gh run` rather than polling
with sleeps.
