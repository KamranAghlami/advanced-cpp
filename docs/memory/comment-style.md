---
name: comment-style
description: How Kamran wants source comments in this repo written — tutorial-quality but to the point, not exhaustive.
metadata:
  type: feedback
---

Source comments here should read like a professional tutorial book: explain the
*technique* and the *why*, including the failure a line prevents. But keep them
to the point — do not over-comment, and do not restate what the code says.

**Why:** this repo is a teaching artifact, so a bare implementation is worth
little; but a wall of prose is worse than none, because nobody reads it and it
rots against the code.

**How to apply:** comment the non-obvious decision, the rejected alternative, and
the measured result. Skip the accessor wall, the obvious loop, and anything a
reader can see. Prefer a `static_assert` or a test over a paragraph — a claim the
compiler re-checks cannot go stale. See [[measure-dont-assert]].
