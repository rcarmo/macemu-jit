# AArch64 JIT Barrier Classes

## Purpose

This document classifies the kinds of operations and transitions that must be treated as barriers in the BasiliskII AArch64 JIT.

A barrier is any point where native code must stop assuming it exclusively owns architectural state.

This taxonomy exists so barrier decisions are made from semantics, not from whichever PC is currently at the frontier.

---

## Barrier levels

## Class B0 — No barrier

Safe for ordinary native continuation.

Conditions:

- no helper call with architectural side effects
- no privileged or supervisor-state effect
- no trap or exception side effect
- no need to rebuild architectural PC state for downstream correctness
- no reliance on interpreter-visible lazy state remaining valid beyond current local scope

Typical examples:

- straightforward ALU ops with correct flag handling
- simple register moves
- simple memory ops already proven exact in current backend contract

## Class B1 — Materialization barrier, native continuation allowed only by proof

Native continuation may still be allowed, but only after required state materialization.

Conditions:

- helper or emitted path may expose state to later code in a way that requires exact PC/flag/register writeback
- no non-local control transfer, but state is now visible outside the purely local instruction window

Typical examples:

- instruction families where lazy flags or PC-relative state must be fully synchronized before the next block-local optimization is legal
- transitions that are locally safe but not chain-safe under stale state

## Class B2 — Exact helper + mandatory block end

This is the preferred classification for difficult, stateful, or still-uncertain instruction families.

Conditions:

- helper can mutate architectural state in ways not fully modeled in current native continuation
- helper can raise/route exceptions
- helper can alter privilege, stack-bank, or PC semantics
- correctness is established only if execution re-enters the dispatcher after the helper

Typical examples in current Basilisk work:

- full SR operations
- MV2SR-style paths
- A-line trap handling
- exact runtime-helper containment for known optlev=2 blocker families

## Class B3 — Interpreter-only barrier

The JIT must end the block and let the interpreter/shared runtime own execution.

Conditions:

- native semantics are not yet trusted enough even through an exact helper continuation path
- or a shared interpreter path is intentionally the semantic authority for this class

Typical examples:

- EMUL_OP / similar emulator-service paths where current contract intentionally re-enters interpreter logic
- any opcode family explicitly designated as not-native-ready

---

## Current classification guidance

## 1. Full SR / privilege-affecting operations

**Current class:** B2

Rationale:

- can alter privileged state
- can alter stack-bank and supervisor-visible execution state
- exact semantics matter more than speculative continuation

Required behavior:

- exact helper path
- mandatory block termination
- dispatcher re-entry from helper-updated architectural state

## 2. Trap-style control flow (A-line, illegal/trap-like semantic classes)

**Current class:** B2 or B3 depending on implementation maturity

Rationale:

- non-local control transfer
- exception vector semantics
- PC ownership must become architectural immediately

Current Basilisk tendency:

- prefer exact runtime helper with explicit block end if available
- otherwise interpreter-only barrier

## 3. EMUL_OP / emulator-service paths

**Current class:** B3

Rationale:

- shared runtime semantics are authoritative
- mixed native continuation is not the intended model here

Required behavior:

- end block
- re-enter interpreter/shared runtime

## 4. Known exact containment families used to keep optlev=2 alive

**Current class:** B2

Examples include current helper-barrier families such as:

- exact helper barriers for difficult MOVE/MOVEA low-level stateful families
- exact helper barriers for specific full-SR / move-to-SR surfaces

Rationale:

- these are not “forever interpreter” classes
- but they are also not yet safe for speculative native continuation

## 5. Straightforward ALU / move / flag-local families already proven by harnesses

**Current class:** B0

Rationale:

- local semantics known
- no stateful helper boundary
- no privilege effects
- native continuation acceptable

## 6. Control-flow families with chain-sensitive state consequences

**Current class:** mixed B1/B2 depending on exact path

Rationale:

- these families are where stale PC / flag / block-state assumptions tend to surface
- even when the branch instruction is semantically correct, the lifecycle consequences may not be

Guidance:

- if the branch/control family can remain native only with an exact, explicit endblock contract, keep it B1 or B2
- do not promote to B0 until chain semantics are fully contract-clean

---

## Current barrier rules for contributors

## Rule 1

If an operation can affect privileged state, it is never B0 by default.

## Rule 2

If a helper can change guest PC or trap routing, assume B2 unless proven otherwise.

## Rule 3

If an instruction family is currently gated only because of downstream block-state corruption, do not treat that as “just a frontier issue.” Classify the family under the boundary semantics it stresses.

## Rule 4

A class may only move downward in barrier level (B3 → B2 → B1 → B0) after:

- the runtime contract for that boundary is explicit
- the relevant golden workloads remain green
- the containment rationale is replaced by a real semantic proof

## Rule 5

A gate without a barrier-class assignment is incomplete.

---

## How to use this taxonomy

For each current or future gate/containment, record:

- semantic family
- barrier class
- why that class is required
- whether it is temporary
- what would prove it can be downgraded

Example template:

```md
### Family: MOVEA.L (An)+,An
- Current class: B2
- Reason: postincrement + architectural register/state interaction still not exact enough for native continuation
- Temporary: yes
- Downgrade proof: exact helper vs native parity on golden workloads + no replacement-frontier regressions
```

---

## Recommended next cleanup

1. annotate current Basilisk containments with B0/B1/B2/B3 labels in comments or a tracking table
2. distinguish permanent semantic exclusions from temporary exact barriers
3. update this file whenever a class changes level

---

## Bottom line

The JIT should stop deciding barriers by folklore.
Barrier choice should be driven by semantic class.

The current safest default is:

- simple exact class → B0
- state-exposing but locally manageable → B1
- difficult or stateful exact helper path → B2
- interpreter-authoritative class → B3

That gives BasiliskII a consistent language for deciding what must end a block and why.
