# BAD IDEAS

This file documents ideas we tested that looked good in theory but had bad practical outcomes.

## 2026-02-21 — Removing libcurl from macOS build (PtrClaw)

### Idea
Unify Linux + macOS HTTP stack by removing the macOS libcurl backend and using one socket+OpenSSL implementation on both platforms.

### Why it seemed good
- One implementation path
- Less platform divergence
- Simpler maintenance

### What happened
For **static release binaries**, macOS size increased massively.

Measured release assets:
- Baseline (`v0.0.2`): `ptrclaw-macos-aarch64` = **526,888 B**
- Unified backend test (`v0.0.2-unify-size-test`): `ptrclaw-macos-aarch64` = **5,102,600 B**

Impact:
- **+4,575,712 B**
- ~**9.68x** larger (about **+868%**)

Linux static artifact size did not improve further from this change.

### Decision
**Do not remove libcurl from macOS path** (for now).
Keep current platform split to preserve small macOS static binary size.

### Follow-up (if revisited later)
Only revisit if we can prove a macOS alternative that keeps static size near current baseline.
