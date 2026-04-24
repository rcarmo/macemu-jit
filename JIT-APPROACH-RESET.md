# MacEmu JIT Approach Reset

> This document is an internal engineering plan for how JIT work in this repository needs to change.
> It intentionally focuses on our code, our workflows, and our invariants.

## Executive summary

Our JIT work is producing real progress, but the current approach is still too frontier-driven, too seam-heavy, and too dependent on tactical containment.

That is acceptable during bring-up. It is not acceptable as the long-term architecture.

The main change we need is this:

**stop treating the JIT primarily as a growing set of opcode successes, and start treating it as a runtime with a strict contract.**

That means:

1. define non-negotiable invariants for register state, flags, block entry/exit, helper calls, and fault recovery
2. audit the code against those invariants centrally, not only at the current frontier
3. demote PC/opcode gating from architectural mechanism to diagnostic tool
4. promote a small set of golden workloads as the canonical proof of progress
5. sequence performance work after state-contract correctness, not before it

If we do this, we can convert the current bring-up effort into a maintainable backend.
If we do not, we will continue to win local battles while carrying too much invisible runtime debt.

---

## Repository-wide diagnosis

### What is already good

This repository already has strong JIT engineering assets:

- targeted harnesses
- interpreter vs JIT comparison discipline
- ROM-level and opcode-level validation
- willingness to bisect exact opcode families and block frontiers
- detailed written investigations instead of vague “seems broken” notes

Those are major strengths.

### What is still wrong in the approach

The current failure mode is not “we are careless.”
It is more specific:

- we are still discovering core runtime rules indirectly through failures
- too much correctness knowledge lives in notes, gates, and remembered bug classes
- the native/interpreter boundary is still treated as an implementation detail in places where it must be a first-class interface
- block chaining and helper fallback behavior are still more empirical than contractual
- some fixes are architectural, but too much of the day-to-day workflow still looks like frontier management

### Current maturity by subsystem

#### BasiliskII 68K AArch64 JIT

Current state is best described as:

- `optlev=1`: viable
- `optlev=2`: advancing, but still under active semantic stabilization
- harness quality: strong
- runtime contract maturity: incomplete

#### SheepShaver PPC AArch64 JIT

Current state is stronger in opcode coverage and harness confidence, but the same warning applies:

- high local correctness in tested surfaces does not yet equal a fully normalized runtime contract
- lessons from the 68K side should be applied before the PPC side accumulates similar seams

---

## The change in approach

## 1. Replace frontier-first thinking with contract-first thinking

Today, much of the work still starts from:

- “what is the next failing block?”
- “which opcode family should we gate?”
- “what moved the frontier deeper?”

That remains useful for diagnosis.
It must stop being the primary mental model.

The primary mental model must become:

- what state is allowed to be virtual?
- what state must be materialized at every boundary?
- who owns PC truth at block entry, at helper call, at block exit, and at fault unwind?
- which flags may remain lazy, and under which exact conditions?
- what does a native block promise the rest of the emulator?

If that contract is explicit and enforced, frontier work becomes faster and safer.
If it is implicit, frontier work remains necessary forever.

---

## 2. Treat gates as probes, not architecture

PC gates, opcode gates, and opt-level containments are useful because they:

- isolate semantic families
- prove causality
- keep boot progress alive while deeper work continues

But they are dangerous when they become normal.

Long-term risks of gate-heavy architecture:

- correctness logic becomes distributed instead of centralized
- performance behavior becomes path-dependent and hard to reason about
- regressions become hard to classify: semantic bug vs wrong gate surface
- future contributors cannot tell which gates are temporary and which are structural

### New rule

A gate may exist in one of only three states:

1. **diagnostic** — temporary, local, used to prove causality
2. **containment** — short-lived, accepted while a central fix is being built
3. **permanent semantic exclusion** — only for instruction classes we explicitly decide not to inline yet

Everything else must migrate into a contract fix.

Every gate must document:

- what invariant it protects
- whether it is diagnostic or permanent
- which central fix should replace it
- what workload proves the replacement works

---

## 3. Promote golden workloads over ad hoc success stories

Harnesses are necessary but not sufficient.
We need a fixed, small set of canonical workloads that represent semantic classes.

### Required workload classes

For BasiliskII 68K:

1. **boot-to-desktop workload**
   - proves block transitions, interrupts, memory model, OS startup behavior
2. **graphics corruption workload**
   - proves memory writes, flags, and mixed helper/native state do not silently diverge
3. **allocator / low-memory state workload**
   - proves stateful routines survive native execution without hidden metadata corruption
4. **ROM harness**
   - proves broad control-flow and primitive semantics
5. **opcode equivalence harness**
   - proves exact semantic parity for targeted instruction classes
6. **performance benchmark**
   - only meaningful after the above are green

For SheepShaver PPC:

1. boot-to-desktop or boot-to-splash canonical path
2. ROM harness
3. opcode equivalence harness
4. graphics/VNC interaction smoke
5. benchmark workload

### Rule

A JIT change is not “good” because it pushes the frontier.
A JIT change is “good” if it improves or preserves the golden workload set.

---

## 4. Move performance work behind state-contract correctness

A mature JIT does not optimize uncertain semantics.
It first stabilizes the model, then removes overhead selectively.

We must separate performance work into two categories.

### Category A: correctness-preserving infrastructure work

Allowed immediately:

- reducing redundant flushes only after proving boundary equivalence
- simplifying state materialization paths without changing behavior
- improving fault recovery overhead after proving recovery semantics
- tightening register allocator bookkeeping if it preserves block contracts

### Category B: speculative speed work

Delay until contracts are stable:

- aggressive block chaining changes
- larger virtual-state windows
- broader lazy-flag elision
- helper bypass paths
- speculative direct stores without shared boundary proof

### New rule

No optimization that weakens observability or boundary discipline should land unless the golden workloads remain green and the affected invariant is documented.

---

## 5. Make fault recovery a first-class subsystem

Memory faults, bus errors, misaligned paths, and helper escapes are not debugging accidents.
They are part of the runtime design.

A native backend is only mature when it has a clear answer to:

- what happens when a compiled block faults?
- where does control resume?
- which registers are authoritative after recovery?
- what is guaranteed about PC, flags, and stack state after unwind?
- can the interpreter safely resume without reading stale virtual state?

This must be handled centrally, not reconstructed from notes.

---

## Non-negotiable invariants

These invariants need to be treated as repository law.

## Invariant 1: there must be exactly one authoritative PC model at each boundary

We may keep different PC representations for performance, but ownership must be explicit.

At minimum we need answers for:

- block entry
- direct block chain
- slow-path helper call
- interpreter fallback
- exception / bus error unwind
- end-of-block dispatch

For each of those, the code must define which of these are valid and synchronized:

- guest PC
- host pointer PC
- previous PC / block origin state

If a path can re-enter the dispatcher or interpreter, it must rebuild the full required PC state before doing so.

## Invariant 2: lazy flags are valid only while their ownership is unambiguous

Flags may remain virtual only if:

- the current block owns them
- no helper requiring materialized flags has been crossed
- no boundary has been taken that can resume elsewhere
- no fallback path can observe stale flag state

If any of those are false, flags must be materialized.

This must not depend on folk knowledge or comments alone.

## Invariant 3: helper calls are semantic barriers unless explicitly proven otherwise

The default assumption for runtime helpers must be:

- they may observe full architectural state
- they may change privileged or latent state
- subsequent native code may not assume continuity unless explicitly modeled

If a helper is not a semantic barrier, that must be proven and documented.

## Invariant 4: block chaining must not bypass validation silently

A direct native jump is only legal if the target block’s assumptions are still valid.

At minimum, the system must define:

- whether the target checksum/state validation still runs
- how stale block state is prevented
- how invalidated code returns to safe dispatch
- which state must be written back before chaining

## Invariant 5: interpreter and JIT builds must not disagree on shared helper semantics

A build with JIT support but native JIT disabled must behave like the clean interpreter for shared logic.

That means:

- no build-time flag semantics drift
- no alternate shared helper behavior that changes guest state before native code even runs
- no `USE_JIT` side effects on interpreter-visible semantics without explicit rationale

## Invariant 6: memory/fault handling must preserve restartability

If a compiled block faults, the runtime must know:

- whether the faulting instruction is restartable
- whether architectural state has been partially committed
- whether retry is legal
- whether fallback to interpreter needs pre-fault or post-fault register state

## Invariant 7: every architectural exception path must choose between exactness and barriering

For tricky classes like:

- SR/CCR writes
- traps
- emulated hardware ops
- privileged state changes
- mixed-size memory semantics

we need a binary choice:

1. exact inline model
2. exact runtime helper plus block barrier

What we cannot tolerate long-term is an ambiguous hybrid that only works because the frontier has not yet reached it.

---

## What needs to change in backend architecture

## A. Create a central JIT contract document and keep it current

We need one canonical technical document covering:

- virtual register model
- PC model
- lazy flag model
- helper ABI expectations
- block entry/exit rules
- fault recovery rules
- allowed fast paths
- prohibited shortcuts

This document should live near the JIT code and be updated whenever a boundary rule changes.

### Proposed file

- `BasiliskII/docs/AARCH64_JIT_RUNTIME_CONTRACT.md`

This new approach-reset document is the policy layer.
The runtime contract document should be the low-level technical layer.

---

## B. Separate “JIT structure” from “native opcode enablement”

We need a stricter distinction between:

- JIT runtime structure being enabled
- native codegen for a specific instruction family being enabled

This distinction already exists informally via optimization levels and gates.
We need it formalized.

### Desired model

- **L0**: pure interpreter
- **L1**: JIT runtime active, but exact interpreter-style execution boundaries
- **L2**: native codegen enabled only for semantically approved classes
- **L3**: future optimization layer after L2 is contract-clean

This matters because a structurally correct L1/L2 boundary makes every future native class cheaper to validate.

---

## C. Build one shared notion of “barrier-worthy” operations

Today, barrier-worthy operations are discovered piecemeal.
That should become a central classification.

### Barrier-worthy classes should include at least

- SR/CCR and privilege-affecting instructions
- traps and emulation ops
- helper calls that may inspect global machine state
- instructions with known self-alias or width-sensitive state interactions
- operations that can reconfigure stack/interrupt/supervisor state

This classification should live in one place and be referenced by both code and docs.

---

## D. Normalize state materialization helpers

We need one clear set of primitives for:

- flush registers
- flush flags
- flush PC state
- rebuild architectural state before helper/interpreter entry
- end block with exact restart state

These should be the only sanctioned way to cross major boundaries.

If multiple ad hoc variants exist, they need to be reduced.

---

## E. Treat allocator, low-memory, and boot-state routines as contract tests

One major lesson from the current BasiliskII work is that low-memory boot/allocator behavior is where “almost correct” JITs expose themselves.

Therefore:

- these routines should not be treated as random boot blockers
- they should be treated as high-value semantic probes

We should explicitly tag such routines in docs and harnesses as:

- state-sensitive
- width-sensitive
- helper-boundary-sensitive

---

## What needs to change in correctness strategy

## 1. Audit centrally before isolating locally

Current pattern:

- isolate failing PC
- gate family
- move frontier

New pattern:

1. identify failure surface
2. ask which invariant class it belongs to
3. audit the central contract seam first
4. only then do local PC/opcode isolation if the central seam is clean

This should reduce time spent chasing downstream symptoms.

---

## 2. Separate “semantic families” from “frontier addresses”

A frontier PC is a symptom location.
An opcode family is a semantic class.

We need to keep those separate in notes and fixes.

### Required note format

For each major issue, record:

- **symptom frontier**: which PCs surfaced the bug
- **semantic family**: which instruction/helper/state class was actually wrong
- **central invariant violated**
- **temporary containment used**
- **final permanent fix**

That makes future archaeology much easier.

---

## 3. Adopt “exact helper barrier” as a legitimate end state for hard classes

Not every instruction needs to be inlined immediately.

For difficult classes, an acceptable mature state is:

- exact runtime helper
- explicit block barrier
- documented reason why full inline semantics are deferred

That is much better than an inline path with uncertain contracts.

---

## 4. Keep interpreter parity tests alive in JIT-enabled builds

The repository already learned that build-time JIT configuration can alter shared semantics.

Therefore we should permanently keep tests for:

- interpreter build
- JIT-enabled but native-disabled build
- JIT-enabled native build

If the first two differ, that is a runtime-contract regression even before native codegen is involved.

---

## What needs to change in performance strategy

## 1. Benchmark only stable surfaces

Benchmarks should be attached to states that are contract-clean.

For example:

- stable L1
- stable L2 families
- stable desktop boot path
- stable benchmark app path

Do not treat performance measurements from unstable gate bundles as architecture guidance.

---

## 2. Recover speed through central fixes, not frontier exemptions

The best speedups will come from:

- fewer forced materializations after proving ownership rules
- better chain validation model
- cleaner helper ABI
- less redundant boundary rebuilding
- fewer “just in case” barriers after exact semantics are proven

The worst speedups will come from:

- removing guards before the contract is understood
- widening lazy-state windows while failures are still structural

---

## 3. Add a maturity ladder to every performance note

Every reported number should say which stage it comes from:

- interpreter
- structural JIT only
- native JIT with containment
- native JIT without frontier gates
- desktop-stable runtime

Without that, benchmarks are too easy to over-interpret.

---

## What needs to change in tooling and process

## 1. Add an invariant checklist to JIT PRs/commits

Every substantial JIT change should answer:

- does this change PC ownership rules?
- does it change flag ownership or flush timing?
- does it alter helper barrier semantics?
- does it affect interpreter/JIT shared code?
- what workloads prove it safe?

## 2. Tag every gate with ownership and expiry intent

Every gate should have:

- why it exists
- which invariant it protects
- whether it is temporary
- how to prove it can be removed

## 3. Reduce critical knowledge in ad hoc notes

The notes we have are valuable, but some of their conclusions need to migrate into canonical docs near the code.

Specifically:

- register ownership rules
- exact barrier-worthy classes
- fault recovery model
- allowed chain fast paths

## 4. Keep one stable “known good” harness preset per subsystem

For BasiliskII:

- one canonical desktop boot preset
- one canonical graphics repro preset
- one canonical ROM harness preset

For SheepShaver:

- one canonical interpreter desktop preset
- one canonical JIT boot preset
- one canonical ROM harness preset

This avoids turning every validation run into a custom experiment.

---

## Exact first five code areas to audit next

These are the first five code areas that should be audited under the new approach.
They are ordered by leverage, not by convenience.

## Audit area 1: global block lifecycle and boundary discipline

### Primary files

- `BasiliskII/src/uae_cpu_2026/compiler/compemu_support_arm.cpp`
- `BasiliskII/src/uae_cpu_2026/compiler/compemu_legacy_arm64_compat.cpp`

### Why first

This is where the runtime decides:

- what a block assumes at entry
- what must be flushed at exit
- when flags matter
- how helper/interpreter fallback is entered
- whether later native code is allowed to continue after stateful operations

### Audit questions

- Are block entry assumptions explicit and minimal?
- Does every exit path materialize the exact state promised by the runtime contract?
- Are helper fallbacks always classified correctly as barriers or non-barriers?
- Does any fast path bypass required validation or materialization?
- Are legacy compatibility helpers still smuggling in assumptions that do not belong in the backend contract?

### Deliverable

A written table of every block exit/fallback path and the exact state each one guarantees.

---

## Audit area 2: PC ownership and pointer-width arithmetic

### Primary files

- `BasiliskII/src/uae_cpu_2026/compiler/compemu_midfunc_arm64.cpp`
- `BasiliskII/src/uae_cpu_2026/compiler/codegen_arm64.cpp`
- `BasiliskII/src/uae_cpu_2026/compiler/codegen_arm64.h`

### Why second

A large class of hard failures comes from:

- stale host-PC state
- partial PC triple updates
- 32-bit truncation in 64-bit address arithmetic
- direct chain paths that assume more than they have proven

### Audit questions

- Which register or memory slot is authoritative for host PC at every phase?
- Which helpers may construct or modify PC values?
- Are all PC-relative arithmetic paths width-clean?
- Are direct chain entry and fallback entry rebuilding the same canonical state?
- Is there any path that updates one PC representation but not the others before crossing into shared runtime code?

### Deliverable

A PC state machine diagram covering block entry, chain, fallback, helper call, fault, and dispatcher re-entry.

---

## Audit area 3: flags, liveness, and selective materialization

### Primary files

- `BasiliskII/src/uae_cpu_2026/compiler/compemu_midfunc_arm64_2.cpp`
- `BasiliskII/src/uae_cpu_2026/m68k.h`
- `BasiliskII/src/uae_cpu_2026/compiler/compemu_support_arm.cpp`

### Why third

We have already proven that flag ownership bugs can masquerade as:

- graphics corruption
- DBRA/branch misbehavior
- allocator divergence
- interpreter/JIT build mismatch

### Audit questions

- Which flag helpers are width-exact for byte/word/long operations?
- When are lazy flags allowed to survive across a boundary?
- Do live/dead decisions ever suppress a flush that a slow path still requires?
- Are build-time JIT and non-JIT flag semantics identical in shared code?
- Are X/CCR/SR-sensitive operations always using an exact path?

### Deliverable

A matrix of flag producers, flag consumers, and mandatory flush points.

---

## Audit area 4: semantic barrier classification for dangerous instruction classes

### Primary files

- `BasiliskII/src/uae_cpu_2026/compiler/compemu_midfunc_arm64_2.cpp`
- `BasiliskII/src/uae_cpu_2026/compiler/compemu_support_arm.cpp`
- `BasiliskII/src/Unix/compemu.cpp`

### Why fourth

This is where we stop accumulating unexplained gates and instead define why a class is:

- safe to inline
- helper-backed but chain-safe
- helper-backed and barrier-required
- not ready for native L2

### Audit questions

- Which instruction classes are currently “safe by evidence” rather than “safe by contract”?
- Which classes should permanently use exact helper barriers for now?
- Which currently gated classes can be regrouped into a smaller number of semantic exclusions?
- Is the generated code reflecting the same barrier model documented in the runtime contract?

### Deliverable

A canonical barrier-classification table covering SR/CCR, traps, EMUL_OP-style ops, alias-heavy moves, and other stateful families.

---

## Audit area 5: dispatcher, fault unwind, and restartability

### Primary files

- `BasiliskII/src/newcpu.cpp`
- `BasiliskII/src/CrossPlatform/sigsegv.cpp`
- `BasiliskII/src/Unix/main_unix.cpp` *(plus any platform-specific fault/memory glue that participates in JIT recovery)*

### Why fifth

Even a semantically correct block is not safe if the runtime cannot recover from faults and resume from a coherent state.

### Audit questions

- What exact state is valid when a compiled block faults?
- Can the interpreter always resume safely from that state?
- Are there partial-commit windows where guest state is neither pre- nor post-instruction exact?
- Is restart behavior documented per class of fault?
- Are fault paths consistent across BasiliskII and SheepShaver where shared design applies?

### Deliverable

A restartability contract for compiled code, including preconditions for fallback to interpreter after fault.

---

## What we should do immediately after those five audits

1. write `AARCH64_JIT_RUNTIME_CONTRACT.md`
2. write `AARCH64_JIT_BARRIER_CLASSES.md`
3. tag every current gate as diagnostic, containment, or permanent
4. reduce duplicate boundary helpers into one sanctioned path per boundary type
5. refresh the canonical workload list and make it part of change validation

---

## Proposed file additions

To make this approach durable, the repository should gain these documents:

- `JIT-APPROACH-RESET.md` — this document; policy and direction
- `BasiliskII/docs/AARCH64_JIT_RUNTIME_CONTRACT.md` — low-level contract and invariants
- `BasiliskII/docs/AARCH64_JIT_BARRIER_CLASSES.md` — exact barrier taxonomy
- `BasiliskII/docs/AARCH64_JIT_GOLDEN_WORKLOADS.md` — required validation set
- `SheepShaver/docs/AARCH64_JIT_RUNTIME_CONTRACT.md` — PPC-side equivalent once Basilisk rules are stabilized

---

## Definition of done

We should consider the approach reset successful when all of these are true:

1. the core runtime contract is written down and matched by code
2. most surviving gates are documented as temporary probes or exact semantic exclusions
3. golden workloads are stable and routinely used
4. build-with-JIT but run-without-native behaves the same as the clean interpreter on shared semantics
5. performance notes are attached to stable contract states, not unstable frontier states
6. new JIT work can be explained in terms of contract changes rather than folklore

---

## Final recommendation

The repository does **not** need a looser approach or more opportunistic inlining.
It needs a stricter one.

The next phase should not be “enable more opcodes faster.”
The next phase should be:

- define the runtime contract
- audit the five highest-leverage seams
- convert temporary frontier knowledge into explicit architecture
- then resume native enablement from a cleaner base

That is the shortest path from a successful bring-up effort to a durable JIT runtime.
