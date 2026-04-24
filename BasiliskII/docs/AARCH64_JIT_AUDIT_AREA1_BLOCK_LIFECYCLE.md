# AArch64 JIT Audit — Area 1: Block Lifecycle and Boundary Discipline

## Scope

This is the first-pass audit for the highest-leverage JIT seam:

- block creation
- block compilation
- block finalization
- block exit
- helper/fallback barriers
- cache/checksum re-entry
- dispatcher handoff expectations

Primary files audited:

- `BasiliskII/src/uae_cpu_2026/compiler/compemu_support_arm.cpp`
- `BasiliskII/src/uae_cpu_2026/compiler/compemu_legacy_arm64_compat.cpp`
- `BasiliskII/src/uae_cpu_2026/compiler/codegen_arm64.cpp`
- `BasiliskII/src/uae_cpu_2026/compiler/compemu_midfunc_arm64.cpp`
- `BasiliskII/src/uae_cpu_2026/compiler/compemu.h`

This document does not attempt to solve every known bug.
Its purpose is to explain what the current lifecycle model does well, where it is structurally weak, and what should change next.

---

## Executive assessment

### High-level verdict

The current JIT lifecycle is **functional but over-distributed**.

It already contains several strong contract-preserving mechanisms:

- explicit block metadata and statuses
- dependency tracking between compiled blocks
- exact helper-barrier paths for dangerous semantic classes
- cross-block flag preservation fixups
- dispatcher-side PC corruption recovery

But it still has a serious architectural weakness:

> **the block lifecycle contract is spread across too many sites, and some of the most important guarantees are enforced indirectly rather than by one clear boundary model.**

The biggest example is hot chain PC coherence:

- block-end helpers persist `regs.pc_p`
- full PC-triple persistence on hot chain is present but disabled
- dispatcher-side recovery (`execute_normal()` and `check_for_cache_miss()`) compensates for some bad states

That is survivable during bring-up.
It is not a clean final architecture.

---

## What is good already

## 1. `blockinfo` is the right central unit of execution identity

`compemu.h` gives each block a real lifecycle object with:

- identity (`pc_p`)
- compilation mode (`optlevel`)
- activity/validation state (`status`)
- successor dependencies (`dep[]`, `deplist`)
- both validated and direct handlers
- liveness-related state (`needed_flags`)

That is strong infrastructure.
It means the runtime already has the right place to store lifecycle decisions.

### Consequence

We should centralize more contract logic into this lifecycle model rather than leaving it scattered across emit-time heuristics.

---

## 2. The runtime already distinguishes validated and hot execution paths

The split between:

- `handler_to_use`
- `direct_handler_to_use`

is correct and important.

It acknowledges that:

- a block can be executable
- but not necessarily chain-safe
- or chain-safe only under current validation assumptions

The support code around:

- `set_dhtu()`
- `block_check_checksum()`
- `called_check_checksum()`
- `match_states()`

shows a healthy understanding that direct execution and validated execution are different lifecycle states.

### Consequence

This distinction should be elevated into the explicit contract, not treated as an implementation detail.

---

## 3. Helper barriers are one of the cleanest parts of the current lifecycle model

The runtime helper barrier pattern in `compemu_support_arm.cpp` is good engineering.

The pattern is:

1. run exact helper
2. mark that helper-updated PC/state must terminate the block
3. end the block from helper-updated architectural state
4. re-enter dispatcher cleanly

That is far healthier than trying to speculate through stateful helper effects.

The current implementation of `jit_force_runtime_pc_endblock` is a model worth generalizing, not a special-case embarrassment.

### Consequence

This barrier model should become the default approach for difficult semantic classes until their inline form is fully contract-clean.

---

## 4. The cross-block flag fix is architecturally significant

`flush(int save_regs)` forcing `live.flags_are_important = 1` when `save_regs` is true is not just a tactical patch.
It is a real lifecycle rule:

- block-local liveness analysis is not enough at a block boundary
- the next block may require flags even if the current block no longer does

This is exactly the kind of rule that belongs in a lifecycle contract.

### Consequence

The code already knows an important truth:

> some state ownership decisions are valid only inside a block and must be overridden at boundaries.

That truth should be made central.

---

## Where the lifecycle model is weak

## 1. PC coherence on hot chains is still a distributed fix, not a single guarantee

This is the most important weakness.

### What the code does now

In `codegen_arm64.cpp`:

- `compemu_raw_endblock_pc_inreg()` updates countdown, checks `spcflags`, and persists `regs.pc_p`
- `compemu_raw_endblock_pc_isconst()` does the same for constant successor PCs

But in both cases, the fuller PC-triple writeback logic is present and disabled in comments.

So the hot chain path currently relies on:

- `regs.pc_p` being good enough for the next step
- later dispatcher/recovery code to reconstruct or repair the rest when needed

### Why that is structurally weak

The block lifecycle should define what state a direct chain transfers.
Right now, part of that answer is “enough for now, plus later repair if needed.”

That leads to several bad outcomes:

- PC correctness depends on which path consumes the next state first
- some mixed-mode paths are safe only because later code is defensive
- it becomes hard to know whether a block-end optimization is actually legal

### Audit conclusion

Hot chain PC ownership is **not yet fully normalized**.
It is the most important lifecycle contract seam to fix.

---

## 2. Dispatcher entry still acts as a repair station for normal execution

`execute_normal()` in `compemu_legacy_arm64_compat.cpp` does more than dispatch.
It also:

- handles bad `pc_p`
- re-derives `pc_p` from `regs.pc`
- patches up certain corrupt or unmapped-PC situations
- triggers bus error logic for guest PCs outside valid executable ranges

Likewise, `check_for_cache_miss()` contains bad-`pc_p` repair logic.

### Why this matters

Defensive recovery is necessary.
But if normal chain/fallback behavior depends on these repairs to stay coherent, then the lifecycle contract is not tight enough upstream.

### Audit conclusion

The dispatcher is currently doing two jobs:

1. legitimate validation/re-entry
2. partial recovery from upstream lifecycle looseness

Those should be disentangled over time.

---

## 3. `compile_block()` currently owns too many policy classes at once

`compile_block()` does all of the following:

- dependency harvesting
- liveflags propagation
- opt-level selection
- semantic containment routing
- helper barrier selection
- native vs interpreter path choice
- endblock strategy choice
- successor dependency creation
- final handler/status installation

That is too much policy in one function.

### Why this matters

When a single function decides both:

- runtime structure
- and semantic exclusions

it becomes harder to separate:

- “this block must end this way because of lifecycle law”
- from
- “this opcode is temporarily contained because we have not proven it safe yet”

### Audit conclusion

`compile_block()` is carrying too much architectural meaning.
It should still orchestrate compilation, but some of its current responsibilities need to move into explicit classifiers and lifecycle helpers.

---

## 4. Status transitions are meaningful, but not documented as contract transitions

The block statuses:

- `BI_INVALID`
- `BI_ACTIVE`
- `BI_NEED_RECOMP`
- `BI_NEED_CHECK`
- `BI_CHECKING`
- `BI_COMPILING`
- `BI_FINALIZING`

are not just cache internals.
They encode lifecycle guarantees.

Example:

- `BI_NEED_CHECK` means a block exists, but its continued use depends on content validation
- `BI_NEED_RECOMP` means no old compiled assumptions should remain authoritative
- `BI_ACTIVE` means current handlers are live under current validation state

### Problem

Those meanings are real, but implicit.

### Audit conclusion

Status transitions should be documented as part of the lifecycle contract, not inferred from surrounding code.

---

## 5. Dependency handling is good infrastructure, but the contract around it is still underspecified

The block system already has proper successor dependency infrastructure:

- `create_jmpdep()`
- `remove_deps()`
- `set_dhtu()` updates dependent jumps when target direct handlers change

This is strong.

But one important detail reveals the current contract is still not fully centralized:

- `compile_block()` captures successor `needed_flags` before `remove_deps()` because that information is still needed for cross-block flag correctness

That is correct behavior, but it shows the block lifecycle contract is partly encoded as “careful ordering in compile_block” rather than “single explicit handoff rule.”

### Audit conclusion

Dependency tracking is good.
Its relationship to state handoff rules needs to be made explicit.

---

## Concrete boundary inventory

This section summarizes the lifecycle boundaries actually visible in code.

| Boundary | Current mechanism | Health | Main concern |
|---|---|---|---|
| New dispatcher entry | `execute_normal()` | Medium | also acts as repair logic |
| Cache validation entry | `check_for_cache_miss()` / `check_checksum()` | Medium | depends on architectural PC coherence |
| Recompile entry | `recompile_block()` | Medium | assumes dispatcher entry can normalize state |
| Interpreter-only block path | `compemu_raw_exec_nostats()` | Medium | requires exact architectural state before handoff |
| Native block hot exit (PC in reg) | `compemu_raw_endblock_pc_inreg()` | Medium-low | only `regs.pc_p` persisted on hot path |
| Native block hot exit (PC const) | `compemu_raw_endblock_pc_isconst()` | Medium-low | same PC-triple seam |
| Exact helper barrier | `jit_force_runtime_pc_endblock` path | High | currently healthiest boundary |
| Cross-block flag handoff | `flush(save_regs=1)` + successor `needed_flags` | Medium-high | good rule, but distributed |

---

## Strongest first-pass findings

## Finding 1: the helper-barrier lifecycle is healthier than the direct-chain lifecycle

This is the clearest result from the audit.

The helper-barrier model:

- explicitly ends ownership
- materializes required state
- re-enters from authoritative runtime state

The hot-chain model is faster, but currently less explicit and more dependent on downstream repair.

### Meaning

When in doubt, new difficult semantic classes should prefer exact helper barrier until hot-chain state ownership is cleaner.

---

## Finding 2: direct chaining still lacks one fully trusted statement of what state it transfers

The code has several partial answers, but not one single authoritative answer.

### Meaning

Before more aggressive chaining work lands, we need a single definition of:

- which PC fields must be valid
- whether flags must be materialized
- whether target validation is bypassed or preserved
- what fallback later code is allowed to assume

---

## Finding 3: block lifecycle correctness is already partially encoded in fixes that should be promoted to doctrine

Two examples:

- forcing flag importance in `flush(save_regs=1)`
- capturing successor `needed_flags` before dependency removal

These are not random local fixes.
They are early expressions of the real lifecycle contract.

### Meaning

We should promote these into documentation and then simplify code around them, not leave them as isolated “important comments.”

---

## Recommended changes

## Priority 1: centralize the block exit contract

Create one explicit definition covering:

- direct chain with PC in register
- direct chain with constant PC
- slow exit to dispatcher
- helper barrier exit
- interpreter-only subpath exit

This contract should answer, for each exit type:

- what PC state is materialized
- whether flags are materialized
- whether target validation is preserved
- whether further native continuation is legal

## Priority 2: stop relying on dispatcher-side PC repair for normal hot-path coherence

Keep repair logic for corruption.
But the normal chain path should not need dispatcher heuristics to reconstruct ordinary coherent state.

## Priority 3: split lifecycle policy from semantic containment policy

`compile_block()` should continue orchestrating compilation, but the following should move into dedicated classifiers/helpers:

- barrier-worthy instruction classes
- permanent semantic exclusions
- temporary diagnostic containments
- structural exit selection rules

## Priority 4: document block statuses as lifecycle states

Each `blockinfo.status` should have a short documented contract meaning.

## Priority 5: record boundary obligations next to the codegen helpers

`compemu_raw_endblock_pc_inreg()` and `compemu_raw_endblock_pc_isconst()` should carry explicit comments about the exact architectural guarantees they are expected to provide.

---

## Immediate next actions

1. write and keep `AARCH64_JIT_RUNTIME_CONTRACT.md` current
2. add a follow-on document for barrier classes
3. tighten the direct-chain PC contract
4. reduce lifecycle policy hidden inside `compile_block()`
5. re-check golden workloads after each lifecycle change, not only after semantic containment changes

---

## Bottom line

The current block lifecycle is already good enough to support serious JIT progress.
But it is not yet explicit enough to support a long-term clean runtime.

The main architectural task now is not “compile more.”
It is:

- make block boundaries explicit
- make direct-chain transfer rules authoritative
- make helper barriers the deliberate exact path they already want to be
- stop leaving lifecycle truth distributed across codegen helpers, compile logic, and dispatcher repair

That is the right next step for this backend.
