# AArch64 JIT Golden Workloads

## Purpose

This document defines the canonical workloads that validate the BasiliskII AArch64 JIT.

A JIT change is not good because it moves the current frontier.
A JIT change is good if it improves or preserves these workloads.

---

## Workload 1: Interpreter parity in JIT-enabled build

**What it tests**:

- shared helper/interpreter semantics do not change merely because JIT support is compiled in
- build-with-JIT but run-without-native stays aligned with the clean interpreter model

**Why it matters**:

This repository has already seen build-time JIT enablement change shared semantics indirectly.
That is a contract violation even before native codegen is involved.

**Pass condition**:

- no semantic drift between clean interpreter behavior and JIT-enabled interpreter behavior on agreed test surfaces

**Status**:

Must remain mandatory.

---

## Workload 2: Opcode equivalence harness

**What it tests**:

- exact semantic parity for targeted instruction classes
- register/state/flag equivalence between interpreter and JIT execution

**How to run**:

```bash
cd /workspace/projects/macemu
./jit-test/run.sh
```

**Pass condition**:

- all enabled vectors pass
- no new mismatch appears in previously green vectors
- no JIT-only crash in harness mode

**Notes**:

This is the canonical proof for local instruction semantics.
It is not sufficient on its own for whole-runtime correctness.

---

## Workload 3: ROM harness

**What it tests**:

- broad control-flow and instruction-surface correctness
- whether compiled execution can survive real ROM code without depending on UI/hardware side effects

**How to run**:

Use the repository’s current ROM-harness workflow / scripts for BasiliskII.

**Pass condition**:

- no crash
- stable or improved ROM coverage
- no regression in previously stable block classes

**Notes**:

ROM coverage is an important trend signal, but not by itself proof of safe desktop/runtime behavior.

---

## Workload 4: Boot-to-desktop workload

**What it tests**:

- block transitions
- interrupt/timer cadence
- PC ownership across block boundaries
- memory model
- stateful OS startup behavior

**Pass condition**:

- Mac OS boots to desktop on the canonical boot disk image
- no crash
- no permanent wrong-state hang introduced by the JIT change

**Notes**:

This is the most important whole-system workload.
If a change regresses this workload, it must be treated as a runtime-contract regression even if the opcode harness improves.

---

## Workload 5: Graphics corruption workload

**What it tests**:

- memory writes into real framebuffer paths
- flag correctness in stateful routines
- interpreter/JIT parity in rendering-sensitive code

**Current canonical example class**:

- titlebar/menu-chrome corruption class already investigated in this repo

**Pass condition**:

- watched pixels match the clean interpreter/no-JIT reference
- no extra overwrite appears in the real VRAM path
- screenshots remain visually identical or acceptably equivalent

**Notes**:

This workload exists because rendering corruption is often a downstream symptom of deeper state/flag bugs.

---

## Workload 6: Allocator / low-memory state workload

**What it tests**:

- stateful boot/allocator/list-management routines
- pointer-family correctness in guest state
- low-memory and metadata mutation discipline

**Current canonical example class**:

- allocator/free-list divergence work already identified in repository notes

**Pass condition**:

- no wrong-family pointer drift in the known low-memory state paths
- no guest heap/allocator metadata corruption introduced by compiled execution

**Notes**:

This is the canonical probe for “almost correct but still poisonous” JIT behavior.

---

## Workload 7: Performance benchmark

**What it tests**:

- real delivered speedup on a stable contract state

**Examples**:

- Speedometer / graphics benchmark style workloads
- dispatch/compile counters only after correctness workloads are green

**Pass condition**:

- benchmark runs to completion
- performance is reported together with maturity level

**Rule**:

Do not use unstable frontier builds as the basis for architectural performance conclusions.

---

## Maturity ladder

A change is mature only when it preserves all lower levels.

```text
L0  Clean interpreter baseline stable
L1  JIT-enabled interpreter parity stable
L2  Opcode equivalence harness stable
L3  ROM harness stable
L4  Desktop boot stable
L5  Graphics + allocator/low-memory workloads stable
L6  Performance benchmark meaningful
```

### Interpretation

- Moving from L2 to L3 is not enough if L4 regresses
- L6 numbers are only meaningful when L0–L5 are green

---

## Change acceptance rules

Every significant BasiliskII JIT change should state which workloads were checked.

### Minimum required by change type

#### Local opcode semantic change
- Workload 2
- relevant subset of Workload 3

#### Boundary / block-exit / chaining change
- Workloads 2, 3, 4
- and usually 5 or 6 if prior evidence touched those areas

#### Flag/liveness change
- Workloads 2, 3, 5, 6

#### Fault/restart/recovery change
- Workloads 3, 4, 6

#### Performance-motivated change
- all correctness workloads first, then Workload 7

---

## Current missing discipline

The repository already has the right ingredients, but they are still too dispersed across:

- bring-up notes
- audit notes
- memory notes
- ad hoc frontier experiments

This file exists to make the validation loop explicit.

---

## Recommended current canonical set for BasiliskII

Until refined further, the working canonical set should be:

1. `jit-test/run.sh` opcode equivalence harness
2. BasiliskII ROM harness / ROM coverage workflow
3. one canonical desktop-boot preset
4. one canonical graphics-corruption repro preset
5. one canonical low-memory/allocator-sensitive repro preset
6. one canonical performance benchmark preset

The exact scripts/prefs should be named and frozen once the next audit/docs pass completes.

---

## Bottom line

The BasiliskII JIT is ready to stop being judged only by frontier movement.
From now on, it should be judged by whether it preserves the golden workloads in the maturity ladder above.
