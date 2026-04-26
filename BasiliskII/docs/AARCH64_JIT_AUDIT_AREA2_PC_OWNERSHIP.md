# AArch64 JIT Audit — Area 2: PC Ownership and Pointer-Width Arithmetic

## Scope

This is the second-pass audit for the next highest-leverage BasiliskII JIT seam:

- PC ownership
- host-pointer vs guest-PC translation
- pointer-width arithmetic
- block-entry PC assumptions
- endblock PC handoff rules
- dispatcher-side recovery from PC corruption

Primary files audited:

- `BasiliskII/src/uae_cpu_2026/compiler/compemu_support_arm.cpp`
- `BasiliskII/src/uae_cpu_2026/compiler/compemu_midfunc_arm64.cpp`
- `BasiliskII/src/uae_cpu_2026/compiler/codegen_arm64.cpp`
- `BasiliskII/src/uae_cpu_2026/compiler/compemu_legacy_arm64_compat.cpp`
- `BasiliskII/src/uae_cpu_2026/compiler/compemu.h`
- `BasiliskII/src/uae_cpu_2026/registers.h`

---

## Executive assessment

### High-level verdict

The current AArch64 JIT has the right **pieces** for a correct PC model, but the ownership rules are still too distributed and too recovery-dependent.

The implementation already knows several important truths:

- `PC_P` is special and must be treated as a pointer-width value
- 32-bit arithmetic on `PC_P` is a real bug
- block entry should anchor to the current fetch pointer, not stale metadata
- dispatcher and cache-miss paths must defend against bad `pc_p`

However, the code still relies on a hybrid model in which:

- hot chain paths often persist only `regs.pc_p`
- `regs.pc` / `regs.pc_oldp` may remain stale
- downstream dispatcher code repairs or reconstructs state when the seam leaks

That means the repository has a **usable** PC model, but not yet a fully normalized one.

---

## The current PC representations

The BasiliskII AArch64 JIT uses multiple PC representations simultaneously.

## 1. Architectural guest PC

- `regs.pc`

This is the canonical guest-visible PC value.

## 2. Architectural host-pointer PC

- `regs.pc_p`

This is the current host pointer into guest instruction memory.

## 3. PC base for guest-PC reconstruction

- `regs.pc_oldp`

This participates directly in guest-PC derivation.

In `registers.h`:

```c
static inline uaecptr m68k_getpc (void)
{
    return regs.pc + ((char *)regs.pc_p - (char *)regs.pc_oldp);
}
```

So if `regs.pc_oldp` is stale while `regs.pc_p` changes, `m68k_getpc()` becomes wrong.

## 4. Compile-time current host PC

- `comp_pc_p`

Used during block compilation/codegen.

## 5. Block-entry anchors

- `start_pc_p`
- `start_pc`

Used to define block trace identity and compilation anchor.

## 6. Virtual JIT PC register

- `PC_P`

This is the special virtual register used in the JIT backend for pointer-width PC tracking.

---

## What is already correct

## 1. `PC_P` is explicitly treated as special in the backend

This is good and necessary.

Examples visible in current code:

- `compemu_arm.h`: `PC_P` is explicitly singled out
- `compemu_midfunc_arm64.cpp`: add/sub helpers special-case `PC_P`
- pointer-width preservation rules are already documented in comments and code paths

### Audit conclusion

The backend already understands that `PC_P` is not “just another guest register.”
That is a strong foundation.

---

## 2. Pointer-width arithmetic bugs have already been taken seriously

The code already contains targeted fixes for:

- 64-bit preservation when destination is `PC_P`
- avoiding 32-bit truncation in `add_l` / `add_l_ri` style helpers
- separate `uintptr` usage for host-pointer state

### Audit conclusion

This part of the PC model is conceptually sound.
The remaining problems are less about “do we know pointers are 64-bit?” and more about “which PC representation is authoritative at which boundary?”

---

## 3. Block trace anchoring was already moved toward the real fetch pointer

In `execute_normal()` the traced block anchor is explicitly tied to the current fetch pointer:

- `start_pc_p = regs.pc_p`
- `start_pc = get_virtual_address((uae_u8*)regs.pc_p)`

and comments already note that stale `regs.pc` / `regs.pc_oldp` metadata can survive across mixed-mode transitions even when `regs.pc_p` is correct.

### Audit conclusion

This is the right direction: block identity should be anchored to the actual fetch location, not inferred from potentially stale metadata.

---

## Where the PC model is still weak

## 1. Hot endblock helpers still only persist `regs.pc_p` on the fast path

This is the core weakness.

### Current behavior

In `codegen_arm64.cpp`:

- `compemu_raw_endblock_pc_inreg()` updates only `regs.pc_p` on the hot path
- `compemu_raw_endblock_pc_isconst()` also updates only `regs.pc_p` on the hot path

The fuller PC-triple store is present but disabled in comments.

### Why this matters

Because `m68k_getpc()` depends on:

- `regs.pc`
- `regs.pc_p`
- `regs.pc_oldp`

If only one of those changes, guest-PC reconstruction can drift.

### Current consequence

The runtime must rely on:

- block-local paths not needing `m68k_getpc()` immediately
- dispatcher re-entry repair logic
- exact helper barriers for dangerous classes
- cautious validated successor entry

That can be enough to keep progress alive, but it is not a clean steady-state design.

### Audit conclusion

Hot chain PC transfer is still only partially materialized.
That is the main unresolved seam in this audit area.

---

## 2. Dispatcher-side repair is compensating for upstream looseness

Both `execute_normal()` and `check_for_cache_miss()` contain bad-`pc_p` repair logic.

That logic:

- checks whether `regs.pc_p` lands in a valid host memory window
- re-derives `regs.pc_p` from `regs.pc`
- rebuilds `regs.pc_oldp`
- sometimes routes unmapped guest PCs to a bus error

### Why this matters

This is necessary as a safety net.
But it should not be carrying normal hot-path coherence as a routine responsibility.

### Audit conclusion

The dispatcher currently acts as both:

1. a legitimate validated entry path
2. a repair station for leaks in PC ownership transfer

That second role needs to shrink.

---

## 3. `compemu_raw_set_pc_i()` still updates only `regs.pc_p`

`compemu_raw_set_pc_i()` currently stores the host-pointer PC into `regs.pc_p`.

It does not rebuild the full triple by itself.

### Why this matters

Any path that assumes `raw_set_pc_i()` alone creates full canonical PC state is too optimistic.

### Audit conclusion

This helper’s contract should be treated as narrow:

- it sets architectural host-pointer PC
- it does **not** by itself fully canonicalize all guest-PC representations

---

## 4. Constant-successor chains were too eager about direct target handlers

This was the most actionable PC-ownership issue already addressed in code.

### Previous behavior

Constant-successor branch chains could default toward the target block’s direct handler.
That made hot-chain performance attractive, but it also increased dependence on incomplete PC/state transfer.

### Current change

The repository now prefers the validated/non-direct successor handler by default on ARM64 unless explicitly overridden.

This reduces the exposure of constant-successor chains to the weakest part of the current PC-transfer model.

### Audit conclusion

This is the right first contract-first move:

- not a final performance answer
- but a correct default while PC ownership is still being normalized

---

## Concrete ownership matrix

This is the current practical ownership picture.

| Context | Authoritative PC source | Confidence | Notes |
|---|---|---|---|
| Inside local compiled code | `PC_P` / `comp_pc_p` | High | when no boundary crossed |
| Block entry tracing | `regs.pc_p` → `start_pc_p/start_pc` | Medium-high | better than metadata-derived entry |
| Hot const/inreg endblock | `regs.pc_p` only | Medium-low | triple not fully materialized |
| Validated dispatcher entry | repairable from architectural state | Medium | safe, but still compensating |
| Helper barrier exit | helper-updated architectural state | High | healthiest current PC boundary |
| Interpreter fallback after weak hot chain | mixed | Medium-low | depends on downstream repair and not immediately reading stale triple |

---

## Strongest findings

## Finding 1: the repository already knows the PC problem is not pointer width alone

The code and docs already reflect that the real issue is:

- **ownership and synchronization**, not merely 64-bit arithmetic correctness

That is important because it means the next work should focus on boundary discipline, not on searching for more generic pointer-width fixes.

---

## Finding 2: validated successor entry is the correct default until the hot-chain triple is normalized

This is the clearest actionable conclusion.

Since hot chain only updates `regs.pc_p`, and the full triple remains disabled, validated successor entry is the safer contract-first default.

That does not mean direct chains are permanently wrong.
It means they are not yet entitled to be the default.

---

## Finding 3: `m68k_getpc()` makes stale `pc_oldp` a first-class risk

Because guest-PC reconstruction is:

```c
regs.pc + (regs.pc_p - regs.pc_oldp)
```

stale `regs.pc_oldp` is not cosmetic metadata corruption.
It directly changes architectural guest-PC results.

That means any path that updates only `regs.pc_p` must be treated with suspicion unless it can prove `m68k_getpc()` cannot be consulted before recovery.

---

## Recommended next changes

## Priority 1: explicitly define the hot-chain PC contract

We need one written statement covering:

- what `compemu_raw_endblock_pc_inreg()` guarantees
- what `compemu_raw_endblock_pc_isconst()` guarantees
- which later paths are allowed to depend only on `regs.pc_p`
- when full triple persistence is mandatory

## Priority 2: stop allowing downstream repair to substitute for normal PC transfer correctness

Recovery logic stays.
But normal fast-path design should not depend on it.

## Priority 3: narrow and document `compemu_raw_set_pc_i()` semantics

Its contract should be documented explicitly as:

- host-pointer PC write only
- not a full triple canonicalizer by itself

## Priority 4: keep validated successor handoff as the ARM64 default until full triple semantics are proven

That has already started in code and should remain the active default.

---

## Bottom line

We do have a clear way forward on PC ownership now.

It is:

1. treat `PC_P` pointer-width correctness as mostly solved
2. treat hot-chain PC **coherence** as the active structural problem
3. keep validated successor entry as default while that seam is normalized
4. only re-promote direct successor chaining after the full PC transfer contract is explicit and proven

This is the right next technical lens for BasiliskII.
