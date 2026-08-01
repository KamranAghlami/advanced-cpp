---
name: commit-directly-to-main
description: Commit straight to main in this repo — do not create a feature branch first.
metadata:
  type: feedback
---

When asked to commit, commit directly to `main`. Do not branch first.

**Why:** Stated on 2026-08-01, after I put the Module 0 setup work on a `module-0-setup`
branch and Kamran had to fast-forward it back. This is a solo practice repo with no remote
and no PR workflow, so a branch per change is ceremony that only strands work off `main`.

**How to apply:** Stage and commit on `main`. Still commit only when asked — this changes
*where* commits land, not *whether* to make them unprompted.
