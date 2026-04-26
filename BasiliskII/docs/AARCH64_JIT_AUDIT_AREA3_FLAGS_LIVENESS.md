# AArch64 JIT Audit — Area 3: Flags, Liveness, and Selective Materialization

## Scope

This audit covers the current ARM64 flags/liveness model used by the BasiliskII JIT, with emphasis on:

- lazy NZCV ownership
- `FLAGX` vs `FLAGTMP` behavior
- `liveflags[]` / `needed_flags` propagation
- `flush()` and `flush_flags()` discipline
- helper-barrier materialization rules
- where the runtime is still conservative rather than normalized

Primary files audited:

- `BasiliskII/src/uae_cpu_2026/compiler/compemu_support_arm.cpp`
- `BasiliskII/src/uae_cpu_2026/compiler/compemu_midfunc_arm.cpp`
- `BasiliskII/src/uae_cpu_2026/compiler/compemu_legacy_arm64_compat.cpp`
- `BasiliskII/src/uae_cpu_2026/compiler/codegen_arm64.cpp`
- `BasiliskII/src/uae_cpu_2026/compiler/compemu.h`

---

## Executive assessment

### High-level verdict

The current ARM64 flags model is **more coherent than the current hot-chain PC model**, but it is still only partially normalized.

What is already structurally good:

- the runtime has an explicit ownership model for lazy flags
- cross-block flush discipline is already stronger than the older bring-up state
- successor `needed_flags` propagation is doing real work
- the code explicitly accounts for `flags_carry_inverted`

What is still not yet clean enough:

- block entry still restores NZCV eagerly rather than only when proven necessary
- helper barriers rely on `flush(1)` as the de facto architectural materialization point
- `dont_care_flags()` is safe only because later block-boundary code force-reasserts importance before flushing
- selective materialization is present, but it is not yet expressed as a small, central runtime policy

### Audit conclusion

This area does **not** currently look like “flags are fundamentally broken.”
It looks like a runtime that already learned the right contract rules, but still implements them in a somewhat distributed and conservative way.

---

## Current flag-state model

The current ARM64 backend tracks flag truth in three forms:

## 1. Hardware NZCV

This is the native lazy-flags state.
Ownership is tracked via:

- `live.flags_in_flags`
- `flags_carry_inverted`

## 2. Memory-resident architectural flags

This is the interpreter/shared-runtime visible form:

- `regflags.nzcv`
- `regflags.x`

Within JIT bookkeeping, these map primarily to:

- `FLAGTMP` → `regflags.nzcv`
- `FLAGX` → `regflags.x`

## 3. Ownership/importance bookkeeping

The runtime also tracks whether flags currently matter to downstream code:

- `live.flags_on_stack`
- `live.flags_are_important`

Despite the legacy name, “on stack” now effectively means “materialized to memory / architectural form.”

---

## What is already correct

## 1. Cross-block flush discipline is explicit

`flush(save_regs=1)` now force-sets:

```c
live.flags_are_important = 1;
```

before `flush_flags()`.

That matters because intra-block liveness can legally decide that later code in the *same* block does not care about flags, while the *next* block may still require them.

### Audit conclusion

This is a real contract rule, not a temporary hack:

> block-local flag deadness is not allowed to erase cross-block architectural truth.

---

## 2. Carry inversion is treated as a first-class correctness issue

`flags_to_stack()` and the snapshot/restore paths do not blindly treat hardware NZCV as architectural truth.
They explicitly normalize carry polarity when `flags_carry_inverted` is set.

### Audit conclusion

This is one of the healthier parts of the ARM64 flags implementation.
Any future cleanup must preserve this behavior centrally.

---

## 3. Successor-driven flag liveness already exists

`compile_block()` computes backward `liveflags[]` and stores the block-entry requirement in:

- `bi->needed_flags`

It also propagates successor block needs when available instead of always falling back to `FLAG_ALL`.

### Audit conclusion

The repository already has the machinery required for selective materialization.
The remaining gap is not “we do not know what flags are needed.”
The gap is “we do not yet use that knowledge everywhere a normalized runtime would.”

---

## 4. Join/drop handling is semantically aware

On branch joins, current code can deliberately:

1. `flush_flags()` first
2. then `dont_care_flags()`

when neither successor needs `FLAG_CZNV`.

That is the right order.
It preserves interpreter-visible architectural truth for slow paths while still letting successor native code avoid a pointless restore.

### Audit conclusion

This is a good example of selective materialization done correctly.

---

## The main remaining weaknesses

## 1. Block entry still restores NZCV conservatively

At current block entry, after `init_comp()`, the runtime reloads hardware NZCV from `regflags.nzcv`.

This is safe.
It is also conservative.

### Why this matters

`bi->needed_flags` already describes what a block needs on entry.
If a block does not require incoming `FLAG_CZNV`, eagerly restoring NZCV is unnecessary work.

### Audit conclusion

The current model is correctness-first but not yet selectively materialized at block entry.
That is a normalization opportunity, not an emergency bug.

---

## 2. `dont_care_flags()` remains a local optimization, not a boundary policy

`dont_care_flags()` is useful inside a block, but it is only safe because later barrier logic repairs the architectural contract by forcing flag importance before full flush.

### Why this matters

That means the runtime policy currently lives in **two places**:

- local deadness decisions
- boundary repair logic

### Audit conclusion

This works, but it is not yet the cleanest ownership story.
The boundary should be the authority, not local optimism.

---

## 3. Helper barriers must enter C with fully materialized state

This audit area overlaps directly with helper semantics.
`flush(1)` already materializes lazy registers and flags before exact helper barriers.
That is good.

But exact helper barriers also need coherent architectural PC state before any helper that may use:

- `m68k_getpc()`
- `MakeSR()` / `MakeFromSR()`
- exception helpers
- addressing logic that derives guest PC from the architectural triple

### Audit conclusion

Helper barriers are not “just a call.”
They are B2 architectural boundaries.
They must materialize the same state that the helper-visible runtime contract expects.

---

## Current low-risk cleanups taken from this audit

The current cleanups from this audit area are:

- keep `flush(1)` as the authoritative lazy-register/lazy-flag barrier
- make exact helper barriers rebuild the **full PC triple** before entering C helper code
- gate block-entry NZCV restore from `regflags.nzcv` on whether the block actually needs incoming `FLAG_CZNV`

These changes do not weaken the flags contract.
They make the boundary behavior match the same architectural-state expectations that the contract already assumes, while removing one unnecessary eager restore path.

Why this belongs in Area 3:

- helper barriers are one of the main places where lazy state stops being local
- `flush(1)` is the moment flags stop being virtual
- exact helper continuation rules are part of the same ownership boundary
- block-entry restore is now closer to true selective materialization

---

## Recommended next cleanup

## 1. Keep `flush(1)` as the only authoritative full barrier while selective restore expands carefully

Do **not** weaken:

- forced flag importance on full flush
- carry-inversion normalization
- helper-barrier materialization rules

## 2. Treat helper barriers as architectural consumers by default

For any new exact helper path, require all of the following to be explicit:

- PC visibility on helper entry
- flag visibility on helper entry
- whether native continuation is allowed afterward
- whether the helper itself changes architectural PC/flags/state

---

## Short contributor rules

## Rule 1

Do not let `dont_care_flags()` decide cross-block truth.
Only block-boundary policy decides that.

## Rule 2

Any path that leaves compiled code ownership must assume lazy flags are no longer private.

## Rule 3

Any helper that can observe architectural state must be entered from fully materialized architectural state.

## Rule 4

Selective materialization is allowed only when the consumer set is explicit.
If in doubt, materialize.

---

## Bottom line

Area 3 is not primarily a story about missing flag saves.
It is a story about **who owns lazy state, and when that ownership must end**.

Current BasiliskII ARM64 code already has the core pieces:

- liveness analysis
- cross-block flag preservation
- carry normalization
- join/drop discipline

The next step is to make those pieces more centralized and less conservative, starting with block-entry selective restore while keeping helper barriers fully architectural.
