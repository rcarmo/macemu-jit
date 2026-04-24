# JIT Style Decision

## Decision

For new JIT work in this repository, the preferred engineering style is the **SheepShaver-style approach**.

That means:

- simpler state ownership
- more explicit architectural writeback
- fewer lazy-state assumptions
- clearer boundaries between native code, helpers, and interpreter paths
- easier interpreter/JIT comparison
- correctness and explainability before aggressive optimization

This is a decision about **engineering approach**, not about discarding existing BasiliskII work.

BasiliskII's current JIT architecture remains in place, but future work on it should bias toward the same values:

- simplify boundaries
- make ownership explicit
- reduce hidden contract surfaces
- prefer exact helper barriers over speculative native continuation where semantics are uncertain

---

## Why this decision exists

The repository currently contains two broad JIT styles.

### Style A — complex cached/lazy JIT runtime

Characteristics:

- virtual register allocator
- lazy writeback
- lazy flags
- block cache with lifecycle states
- dependency-linked chaining
- multiple PC representations
- more optimization headroom
- more contract complexity

This is the style represented by the current BasiliskII AArch64 JIT.

### Style B — simpler direct/native runtime

Characteristics:

- more immediate architectural state updates
- fewer hidden ownership transitions
- simpler block/runtime model
- easier local reasoning
- easier debugging and validation
- lower optimization ceiling initially
- lower semantic risk

This is the style represented by the current SheepShaver PPC JIT work.

The question is not which style can theoretically become faster.
The question is which style is more effective for this repository’s current engineering needs.

The answer is Style B.

---

## Core rationale

## 1. Correctness is still the limiting resource

The main bottleneck in this repository is not the absence of optimization mechanisms.
It is the cost of proving semantics correct across complex boundaries.

When state can remain virtual across:

- block exits
- direct chains
- helper calls
- fallback paths
- fault recovery

then every bug becomes harder to localize and every new optimization requires a stronger runtime contract.

A simpler JIT style lowers the semantic burden.

---

## 2. Debuggability is a first-order design requirement

This repository succeeds when JIT bugs can be:

- reproduced reliably
- reduced to exact semantic families
- compared against interpreter behavior
- explained from first principles

A simpler approach helps because it keeps:

- state ownership more visible
- architectural writeback closer to the operation that produced it
- failure distance shorter between root cause and symptom

That is more valuable to us right now than the last increment of cleverness.

---

## 3. Hidden contracts are the most expensive form of progress

A sophisticated lazy-state JIT can outperform a simpler one, but only when its invariants are:

- explicit
- centralized
- stable
- enforced

Until then, every new optimization risks adding invisible coupling.

The more complex style is not wrong.
It is simply more expensive to carry safely.

For this repository, that expense is still too high to treat as the default model.

---

## 4. Simpler first, sophistication second

A good JIT evolution path is:

1. get semantics right
2. make state ownership explicit
3. lock down boundaries
4. validate on real workloads
5. only then reduce writeback/flush costs selectively

That sequence matches the simpler style better than the more aggressively lazy style.

---

## What this means in practice

## For new JIT work

Default to the simpler style.

That means:

- prefer immediate or early architectural materialization unless a delayed model is clearly justified
- keep helper boundaries explicit
- keep PC ownership simple
- keep flags materialized or readily materializable
- avoid introducing a block cache lifecycle unless measurements justify it
- avoid carrying virtual state across a boundary without a written contract

### Default question for new code

Before introducing a lazy or optimized mechanism, ask:

> Is the simpler explicit version already too slow, or are we optimizing before we have a stable semantic model?

If the semantic model is not already stable, do the simpler thing first.

---

## For BasiliskII

Do **not** interpret this decision as a demand to rewrite BasiliskII into a completely different backend.

BasiliskII already has a large complex JIT machinery investment:

- block cache
- lifecycle states
- dependency tracking
- lazy virtual register model
- lazy flags
- multi-representation PC model

That machinery should remain.

But future work on BasiliskII should follow the **simplification bias**:

- make the contract explicit
- reduce hidden ownership transitions
- prefer exact barriers over speculative continuation when semantics are uncertain
- treat dispatcher recovery logic as safety net, not ordinary control-flow glue
- remove complexity only after the contract is written and verified

### BasiliskII rule

Keep the architecture, but push its behavior toward clearer ownership and cleaner boundaries.

---

## For SheepShaver

SheepShaver should remain the model for future JIT style decisions.

Its current approach is better aligned with repository needs because it favors:

- local reasoning
- explicit register/state updates
- lower semantic distance
- faster bring-up of new instruction families

If future performance work demands more sophistication, that sophistication should be added incrementally and only after the boundary contract is explicit.

### SheepShaver rule

Do not prematurely “upgrade” SheepShaver into a more complex lazy-state architecture unless a measured bottleneck clearly demands it.

---

## Design principles going forward

## Principle 1: boundaries beat cleverness

Every important transition must be more obvious than it is clever.

## Principle 2: explicit ownership beats inferred ownership

The runtime should not depend on downstream code to guess which state is authoritative.

## Principle 3: semantic clarity beats speculative optimization

We should optimize only after we can describe why the path is correct.

## Principle 4: architectural writeback is cheap compared to invisible corruption

Extra materialization is acceptable if it makes correctness easier to prove.

## Principle 5: helper barriers are a strength, not a failure

Exact helper + block barrier is a valid mature answer for difficult semantic classes.

## Principle 6: a gate is not a design

PC/opcode gates may be used diagnostically or as short-term containments, but they are not the architectural end state.

---

## Coding rules implied by this decision

## Rule 1

Do not introduce new lazy-state behavior across a boundary unless the boundary contract is written down.

## Rule 2

Prefer exact helper barriers over speculative native continuation for stateful or privileged operations.

## Rule 3

When adding optimization, prove it against canonical workloads, not just the current frontier.

## Rule 4

Keep interpreter vs JIT parity visible in testing.

## Rule 5

If a simpler implementation is correct and fast enough, prefer it.

## Rule 6

If a more complex implementation is chosen, document:

- ownership model
- flush/materialization obligations
- failure/recovery behavior
- validation workload

---

## Tradeoff statement

This decision does **not** claim that the simpler style has the highest ultimate performance ceiling.

It claims something narrower and more useful:

> For this repository, at this stage, the simpler style is the better default engineering approach.

Why?

Because it delivers the best mix of:

- correctness velocity
- debugging clarity
- maintainability
- confidence in test results
- future contributor accessibility

That is more important right now than maximizing sophistication.

---

## What we keep from the more complex style

Even though the simpler style is preferred, some elements of the more complex model remain valuable and should be reused where already justified:

- structured block metadata
- explicit block status/lifecycle tracking
- selective dependency tracking where chaining exists
- well-defined exact helper barriers
- targeted liveness analysis after the contract is stable

The decision is not “simple everywhere, forever.”

The decision is:

> **simple by default, complexity by proof.**

---

## Final statement

The repository should treat the SheepShaver-style JIT approach as the preferred default model for future work.

For BasiliskII, the action is not to replace the current architecture, but to steer it toward the same values:

- clearer boundaries
- more explicit ownership
- less hidden semantic debt
- correctness before cleverness

That is the engineering stance this repository should adopt going forward.
