# AArch64 JIT Runtime Contract

## Purpose

This document defines the runtime contract for the BasiliskII AArch64 JIT in this repository.

It is not a bring-up diary.
It is not a frontier log.
It is the technical statement of what compiled code, helpers, dispatch paths, and fallback paths are allowed to assume about machine state.

If code violates this contract, the code is wrong even if the current workload happens to boot.

---

## Scope

Primary implementation files:

- `BasiliskII/src/uae_cpu_2026/compiler/compemu_support_arm.cpp`
- `BasiliskII/src/uae_cpu_2026/compiler/compemu_legacy_arm64_compat.cpp`
- `BasiliskII/src/uae_cpu_2026/compiler/compemu_midfunc_arm64.cpp`
- `BasiliskII/src/uae_cpu_2026/compiler/compemu_midfunc_arm64_2.cpp`
- `BasiliskII/src/uae_cpu_2026/compiler/codegen_arm64.cpp`
- `BasiliskII/src/uae_cpu_2026/compiler/compemu.h`
- `BasiliskII/src/uae_cpu_2026/newcpu.cpp`
- `BasiliskII/src/CrossPlatform/sigsegv.cpp`

This document covers:

- block entry and exit
- PC ownership
- lazy flags
- helper barriers
- checksum/cache validation
- dispatcher re-entry
- fault and restart expectations

---

## Terms

### Architectural state

State that the interpreter and the rest of the emulator are allowed to observe directly.
Examples:

- `regs.pc`
- `regs.pc_p`
- `regs.pc_oldp`
- integer register file in `regs.regs[]`
- status/state in `regs.sr`, `regs.s`, `regs.m`, `regs.isp`, `regs.msp`
- memory-resident flag state in `regflags`

### Virtual state

State temporarily held only in native registers or JIT bookkeeping and not yet committed back to architectural storage.
Examples:

- virtual register values tracked in `live.state[]`
- lazy hardware NZCV state tracked through `live.flags_*`
- constant-valued virtual PC state held in the JIT model before flush

### Materialized state

Architectural state that has been written back so that interpreter code, helpers, exception paths, or later blocks may safely observe it.

### Boundary

Any transition where current compiled code can no longer assume it exclusively owns the architectural state.
Examples:

- block exit
- direct chain to another block
- helper call that may inspect or mutate global state
- interpreter fallback
- checksum/check/recompile path
- fault unwind

### Barrier

A boundary that requires compiled code to stop relying on continued native execution assumptions.
Barriers require explicit materialization and re-entry discipline.

---

## Register and state model

## 1. Guest integer registers

Guest integer registers live in two places:

1. architectural storage in `regs.regs[]`
2. virtual JIT state in `live.state[]` plus assigned native registers

The runtime may keep guest values virtual inside a block, but only until a boundary that requires materialization.

## 2. PC model

The PC model has three relevant pieces:

- `regs.pc_p` — host pointer to current guest instruction stream
- `regs.pc` — guest PC value
- `regs.pc_oldp` — base used by guest-PC derivation paths

The JIT may carry PC state virtually during intra-block codegen, but any path that re-enters shared runtime code must guarantee the required PC state is reconstructed.

## 3. Flag model

Flags live in two places:

1. hardware NZCV / carry-derived transient state
2. memory-resident `regflags` state

The JIT tracks this through:

- `live.flags_in_flags`
- `live.flags_on_stack`
- `live.flags_are_important`
- `flags_carry_inverted`

A key rule follows from current implementation:

> lazy flags are only legal while the current block exclusively owns them and no boundary can observe stale state.

## 4. Block metadata model

Each block is represented by `blockinfo`.
Relevant fields:

- `pc_p`
- `count`
- `optlevel`
- `handler`
- `direct_handler`
- `handler_to_use`
- `direct_handler_to_use`
- `needed_flags`
- `status`
- dependency links in `dep[]` / `deplist`

The JIT runtime uses two handler notions deliberately:

- **validated/non-direct path** via `handler_to_use`
- **hot/direct path** via `direct_handler_to_use`

Any chain shortcut that bypasses validation must still satisfy the state contract.

---

## Non-negotiable rules

## Rule 1: every boundary must declare its PC ownership semantics

No path may implicitly assume that `regs.pc_p`, `regs.pc`, and `regs.pc_oldp` are all synchronized.

For every boundary, the implementation must define which of these are authoritative on entry to the next subsystem.

## Rule 2: helper calls are barriers unless explicitly proven otherwise

The default classification for runtime helpers is barrier.

Compiled code may only continue natively after a helper if the helper’s state effects are fully modeled and the post-helper continuation contract is explicit.

## Rule 3: block chaining is only legal after state materialization required by the target path

Direct chaining is not just a control transfer.
It is a transfer of ownership over virtual architectural state.

If the target path or any later fallback requires memory-resident state, the source block must materialize it before chaining.

## Rule 4: block-local liveness must never suppress a flush required by a cross-block consumer

Current code already partially encodes this via:

- successor `needed_flags` propagation in `compile_block()`
- forcing `live.flags_are_important = 1` in `flush(save_regs=1)`

That behavior is not optional. It is part of the runtime contract.

## Rule 5: JIT-enabled interpreter behavior must match clean interpreter behavior in shared semantics

Any build that enables JIT support but runs through interpreter/shared helper paths must preserve interpreter-visible semantics.

Build-time JIT enablement must not silently change:

- flag behavior
- helper behavior
- memory semantics
- architectural state materialization rules

## Rule 6: fault recovery must restart from coherent architectural state, not a partially virtualized snapshot

A compiled block may not fault and then rely on the interpreter to guess what state had already been committed.

The runtime must know whether:

- the instruction is restartable
- the architectural writeback has happened
- the PC state is trustworthy
- fallback is safe

---

## Current boundary inventory

This inventory describes the intended contract for each major boundary.

## A. `execute_normal()` entry

Implementation: `compemu_legacy_arm64_compat.cpp`

### Current role

- dispatcher entry
- block trace builder
- interpreter execution path for non-compiled segments
- recovery point for several slow/validation paths

### Required state on entry

At minimum:

- `regs.pc_p` must refer to current guest fetch location, or be recoverable from architectural PC state
- `regs.pc` must be valid guest PC
- `regs.pc_oldp` must be coherent enough for guest-PC reconstruction paths
- guest integer registers and relevant supervisor/stack state must be architectural
- if flags are needed by interpreter/shared helper code, they must be materialized

### Current implementation note

Current code contains explicit bad-`pc_p` recovery heuristics and unmapped-PC bus-error fallback before normal trace/dispatch work.

That is a useful safety net, but it is also evidence that not all upstream boundaries currently satisfy the contract by construction.

## B. `check_for_cache_miss()`

Implementation: `compemu_support_arm.cpp`

### Current role

- verifies whether current `regs.pc_p` already has a suitable block
- repairs bad `regs.pc_p` in some corruption cases
- can force interpreter fallback path

### Required state on entry

- architectural PC state must be coherent enough to re-derive `regs.pc_p`
- no virtual-only register state may be required by this function

### Contract consequence

Any compiled path that may land in `check_for_cache_miss()` must already have committed state needed for architectural recovery.

## C. `cache_miss()` and `recompile_block()`

Implementation: `compemu_support_arm.cpp`

### Current role

- re-enter compilation or validated dispatcher path when block cache state changes

### Required state on entry

- same requirements as `execute_normal()`
- block-local virtual state must no longer be required

## D. `compemu_raw_exec_nostats()` / interpreter-only subpath

Implementation emitted from `compile_block()`

### Current role

- opt-level 0 or interpreter-style fallback path for a block or instruction

### Required state on entry

- guest register and PC state must be architectural
- any required flags must be architectural or correctly rebuilt before shared interpreter logic observes them

## E. `compemu_raw_endblock_pc_inreg()`

Implementation: `codegen_arm64.cpp`

### Current role

- endblock using already-computed successor host PC in register
- updates countdown
- performs spcflags hot-path check
- persists `regs.pc_p` on slow and hot path
- branches to next handler or returns to `popall_do_nothing`

### Required contract

- if the target path can be reached directly, the source block must already have committed every architectural state the target or any later fallback expects
- persisting only `regs.pc_p` is not sufficient if later code relies on `regs.pc` or `regs.pc_oldp`

### Current implementation note

The current code stores `regs.pc_p` on the hot path, while the fuller PC-triple writeback logic remains disabled in comments.

This means the current runtime depends on other mechanisms to maintain PC coherence for later mixed-mode paths.
That is a known seam and must be treated as such.

## F. `compemu_raw_endblock_pc_isconst()`

Implementation: `codegen_arm64.cpp`

### Current role

- endblock for constant successor host PC
- same countdown/spcflags model as in-reg path
- persists `regs.pc_p`
- direct-branches to target handler chosen by caller

### Required contract

Same as `compemu_raw_endblock_pc_inreg()`.
In addition, any direct target chosen by caller must be valid for the architectural state actually materialized.

### Current implementation note

As with the in-reg variant, only `regs.pc_p` is persisted on the hot chain path in current code.
The older full PC-triple store is present but disabled.

## G. Runtime helper barrier path (`jit_force_runtime_pc_endblock`)

Implementation split between `compemu_support_arm.cpp` and helper emitters

### Current role

- run exact runtime helper
- then force endblock from helper-updated `regs.pc_p`
- explicitly avoid continuing codegen under stale assumptions

### Required contract

- helper is authoritative for guest state update
- compiled block must not continue under assumptions made before helper call
- barrier path must rebuild the exact architectural state expected by dispatcher re-entry

### Current implementation note

This is currently one of the healthiest boundary patterns in the codebase because it explicitly chooses exact helper + block termination rather than speculative native continuation.

---

## Flags contract

## 1. Ownership states

The flags model has three meaningful states:

- **in hardware/live native form**
- **materialized to memory**
- **trash / invalid as a source of truth**

The runtime must never let two different consumers believe they each own the authoritative version.

## 2. Materialization rule

At any boundary where subsequent code may:

- consult interpreter/shared helper logic
- branch using architectural flag semantics later than the current block
- continue into another block whose liveness expectations are not local-only

flags must be materialized.

## 3. Cross-block consequence

Current code explicitly forces `live.flags_are_important = 1` in `flush(save_regs=1)`.
That is not a workaround to be casually removed.
It is the current expression of a real contract rule:

> block-local `dont_care_flags()` decisions are not allowed to erase state needed by the next block.

## 4. Carry inversion consequence

Any path storing or restoring flags must account for `flags_carry_inverted` before treating memory-resident flag state as architectural truth.

---

## PC contract

## 1. Guest-PC derivation rule

Compiled code may carry host-PC truth in virtual form, but any path that can end up in:

- `m68k_getpc()`
- interpreter fallback
- helper code using architectural PC
- cache validation / recompilation logic

must guarantee coherent guest-PC reconstruction state.

## 2. Current seam

Current code anchors traced blocks using the actual fetch pointer and contains several repairs for stale or corrupt PC state.
That is useful, but the desired mature state is stricter:

- hot chain paths should not need downstream heuristics to make PC state coherent
- the dispatcher should validate, not repair routine chain corruption as a normal case

## 3. Immediate rule for contributors

No new fast path may update only one PC representation unless the next boundary explicitly requires only that representation.

---

## Helper contract

## 1. Helper classes

Helpers fall into three classes.

### Class H1 — exact runtime helper + mandatory block barrier

Use when helper may:

- change PC in non-local ways
- change SR/CCR/supervisor/stack state
- raise exceptions
- inspect or mutate memory/state not modeled in current block

### Class H2 — exact helper with native continuation allowed only by proof

Use only when post-helper state ownership is explicitly documented and preserved.

### Class H3 — pure local helper or emitter utility

No architectural boundary effect.

## 2. Current implementation preference

For difficult classes, this runtime should prefer H1 over speculative inline continuation.

---

## Block lifecycle contract

## 1. Block creation

`prepare_block()` establishes per-block dispatcher stubs and initializes block metadata.

Contract:

- new blocks begin invalid
- handler selection is not final until compilation/finalization completes
- dependency links must be reset before recompile

## 2. Block compilation

`compile_block()` is responsible for:

- deriving per-op live flag needs
- choosing opt level and semantic containments
- compiling or routing ops
- establishing successor dependencies
- choosing final endblock behavior
- finalizing handlers and status

Contract:

- compilation may not inherit stale dependency assumptions after `remove_deps()`
- successor flag information must be captured before dependency removal if it influences cross-block flag contract
- final block status must match validation expectations of the cache system

## 3. Active/checking/recompile states

`BI_ACTIVE`, `BI_NEED_CHECK`, `BI_NEED_RECOMP`, `BI_INVALID`, `BI_COMPILING`, `BI_FINALIZING` are runtime states, not just cache hints.

Changing them changes what boundary rules apply.

### Minimum expectations

- `BI_ACTIVE`: handler and direct handler are usable under current validation assumptions
- `BI_NEED_CHECK`: direct target may exist, but content validation must occur before normal use
- `BI_NEED_RECOMP`: execution must not keep using the old compiled body as authoritative
- `BI_INVALID`: no compiled assumptions remain valid

---

## Fault and restart contract

## 1. Required outcome

If compiled code faults or hands off through an exceptional path, the runtime must know whether the interpreter can resume from current architectural state without replay ambiguity.

## 2. Minimum guarantees before fault-sensitive operations

Before any operation that may escape compiled code unexpectedly, the runtime must ensure one of two things:

1. the operation is restartable from current virtual state by design
2. required architectural state has already been materialized

## 3. Current design implication

The project already contains recovery and repair logic for corrupted `pc_p` and unmapped PC cases.
That logic must be treated as part of the fault boundary, not as permission to keep boundary semantics loose elsewhere.

---

## Current known weak seams

These are not hypothetical.
They are current contract weak points visible in the implementation.

## Weak seam 1: PC triple coherence on hot chains

Current hot endblock paths persist `regs.pc_p`, but full PC-triple persistence is still commented out.
This means mixed-mode correctness still depends on downstream recovery or on paths that avoid needing the other PC fields immediately.

## Weak seam 2: distributed boundary policy

Boundary rules currently live across:

- `compile_block()`
- `flush()`
- helper barrier emitters
- `codegen_arm64.cpp` endblock helpers
- `execute_normal()` recovery logic
- cache validation code

That distribution makes contract drift easier.

## Weak seam 3: semantic containment mixed with runtime structure

`compile_block()` currently mixes:

- structural JIT policy
- semantic opcode containments
- diagnostic gates
- helper barrier decisions

That works, but it weakens the visibility of the actual runtime contract.

---

## Contributor checklist

Before changing JIT control flow, helper behavior, or state flushing, answer all of these:

1. Which boundary is being changed?
2. Which PC representation is authoritative before and after that boundary?
3. Are flags still lazy there, and why is that safe?
4. Can interpreter/shared helper code observe stale state on the changed path?
5. Does this path bypass validation that another path expects?
6. If a fault occurs immediately after the change, can fallback still restart coherently?

If any answer is unclear, the change is not ready.

---

## What this contract implies for next work

The immediate next work is not “inline more opcodes.”
It is:

1. centralize block lifecycle rules
2. centralize barrier classification
3. remove dependence on downstream PC repair for normal hot chains
4. keep exact helper barriers for hard classes until their inline semantics are contract-clean
5. validate every significant boundary against canonical workloads

---

## Summary

The AArch64 JIT is allowed to keep state virtual only while it exclusively owns that state.
The instant ownership crosses a block boundary, helper boundary, validation boundary, or fault boundary, the required architectural state must already be materialized.

The current implementation already contains several pieces of this contract.
The next phase of work is to finish making that contract explicit, central, and enforceable.
