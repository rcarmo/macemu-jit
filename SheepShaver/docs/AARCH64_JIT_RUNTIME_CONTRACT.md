# SheepShaver AArch64 JIT Runtime Contract

## Purpose

This document defines the runtime contract for the SheepShaver PPC → AArch64 direct-codegen JIT.

It is not a bring-up diary. It is not a frontier log.
It is the technical statement of what compiled code, the dispatch loop, and fallback paths
are allowed to assume about machine state at every boundary.

If code violates this contract, the code is wrong even if the current workload happens to boot.

---

## Scope

Primary implementation files:

- `SheepShaver/src/kpx_cpu/src/cpu/jit/aarch64/ppc-jit.cpp` — block compiler
- `SheepShaver/src/kpx_cpu/src/cpu/jit/aarch64/ppc-jit.h` — public API
- `SheepShaver/src/kpx_cpu/src/cpu/jit/aarch64/ppc-jit-glue.hpp` — dispatch integration
- `SheepShaver/src/kpx_cpu/src/cpu/jit/aarch64/jit-target-cache.hpp` — RWX cache and icache flush
- `SheepShaver/src/kpx_cpu/src/cpu/ppc/ppc-cpu.cpp` — kpx_cpu execute loop and JIT dispatch
- `SheepShaver/src/kpx_cpu/sheepshaver_glue.cpp` — SIGSEGV handler, test harness

---

## Terms

### Architectural state

State that the interpreter and the rest of the emulator are allowed to observe directly.

- `regs.pc` (at struct offset PPCR_PC = 1052)
- `regs.gpr[0..31]` (offsets 0..124)
- `regs.gpr_hi[0..31]` (offsets 128..252, G5/PPC64 upper halves)
- `regs.fpr[0..31]` (offsets 256..511, 64-bit doubles)
- `regs.cr` (offset PPCR_CR = 1024)
- `regs.xer` as bytes: SO (1028), OV (1029), CA (1030), byte_count (1031)
- `regs.fpscr` (offset PPCR_FPSCR = 1040)
- `regs.lr` (offset PPCR_LR = 1044)
- `regs.ctr` (offset PPCR_CTR = 1048)

### Virtual state

State temporarily held only in ARM64 registers and not yet written back to the struct.
This JIT has **very limited virtual state** — all emitted code reads and writes directly to the
struct via `[RSTATE, #offset]` loads and stores. In practice, values are virtual only for the
duration of the few ARM64 instructions that compute a single PPC instruction's result before
the final store.

### Materialized state

Architectural state written back to the struct so that the interpreter or fallback paths may
safely observe it.

### Boundary

Any transition where current compiled code can no longer assume it exclusively owns the state.

- block exit (epilogue emitted)
- interpreter fallback (compile_one returns false → `emit_epilogue_with_pc`)
- block terminator (blr, b, bclr, bcctr, bc* with Rc)
- EMUL_OP trap (opcode 6 class) — always an incomplete block → interpreter handles
- JIT disabled by SS_USE_JIT gate → interpreter handles entire block

---

## Register and state model

### 1. ABI

Generated blocks are called as:

```c
void compiled_block(void *regs);
```

`x0` on entry = pointer to `powerpc_registers` struct.
The prologue moves x0 to x20 (callee-saved):

```
STP   FP, LR, [SP, #-16]!
STP   x19, x20, [SP, #-16]!   // x20 = RSTATE
STP   x21..x28, ...
MOV   x20, x0
```

The epilogue restores and returns:

```
LDP   x27, x28, [SP], #16
...
LDP   FP, LR, [SP], #16
RET
```

Callee-saved host registers x19–x28 are always preserved across the block boundary.
Scratch registers x0–x2 (RTMP0/1/2) are caller-saved and have no meaning at block entry or exit.

### 2. PC model

There is exactly one PC representation: `regs.pc` at offset PPCR_PC.

**At block entry**: PPCR_PC = block start PC (set by the kpx_cpu interpreter's last `increment_pc`).

**During block execution**: PPCR_PC is NOT updated per-instruction. It retains the block entry value.

**At block exit**: PPCR_PC is written by `emit_epilogue_with_pc(next_pc)` — always the PC of the
next instruction to execute (i.e. the instruction after the last compiled one, or the branch target
for taken branches).

**Contract consequence**: If a fault occurs mid-block, PPCR_PC contains the block entry PC.
The SIGSEGV handler will restart from that PC. The interpreter will re-execute the entire block
cleanly. This is correct (restartability = block-level granularity, not instruction-level).

**Rule**: No path may read PPCR_PC mid-block and assume it reflects the currently executing
instruction. Only the block entry PC is valid mid-block.

### 3. Flag model

**No lazy flags.** This JIT always materializes CR and XER immediately.

- Every instruction with RC bit set calls `emit_update_cr0()` which writes PPCR_CR.
- Every carry-setting instruction calls `emit_write_xer_ca_from_carry()` which writes PPCR_XER_CA.
- FPSCR rounding mode is synced to ARM64 FPCR on `mtfsfi`/`mtfsf`/`mtfsb0`/`mtfsb1`.

**Contract**: At every block boundary, CR, XER.CA, XER.SO, and FPSCR are architectural.
No downstream code needs to account for lazy flag state.

**Rule**: Any new opcode handler that modifies CR, XER, or FPSCR must materialize those values
before the emit function returns. There is no deferred materialization mechanism.

### 4. GPR model

GPRs are read with `emit_load_gpr(rd, n)` → `LDR Wd, [RSTATE, #PPCR_GPR(n)]` at the start
of each handler. They are written with `emit_store_gpr(rs, n)` → `STR Wd, [RSTATE, #PPCR_GPR(n)]`
at the end of each handler. Values are virtual only for the duration of a single handler's
computation (a few ARM64 instructions).

**Contract**: At every block boundary, all GPRs are architectural.

---

## Block lifecycle contract

### Block compiler entry (`ppc_jit_aarch64_compile`)

**Preconditions**:
- `pc` is a valid PPC address within `[ram, ram + ramsize)`
- `ram` and `ramsize` are consistent with the current MAC RAM mapping
- JIT cache has space (`jit_cache_wp < jit_cache_end - 256`)

**What the compiler does**:
1. Emits prologue (callee-save, x20 = regs ptr)
2. Fetches PPC instructions from `ram[pc - (uint32_t)(uintptr_t)ram]`
3. For each instruction, calls `compile_one(op, cur_pc)`
4. If `compile_one` fails: emits `emit_epilogue_with_pc(cur_pc)`, marks `complete = false`, stops
5. If block terminator hit: emits terminator epilogue, stops
6. After loop: if no `RET` emitted yet, emits `emit_epilogue_with_pc(cur_pc)`
7. Flushes icache for the generated code range
8. Returns `ppc_jit_block` with {code, code_size, ppc_start_pc, ppc_end_pc, n_insns, complete}

**Postconditions**:
- Generated code is executable
- `out->complete = true` iff every instruction in the block was compiled natively
- `out->complete = false` iff the block was truncated at an unhandled opcode

### Block execution path

In `ppc-cpu.cpp` (`pdi_execute` label):

```
1. Check SS_USE_JIT gate — if disabled, goto skip_jit
2. Call ppc_jit_aarch64_compile(pc(), RAMBaseHost, RAMSize, &jblk)
3. If compilation failed or !jblk.complete: goto skip_jit (interpreter handles it)
4. Call fn(regs_ptr()) — executes the compiled block
5. Validate PC: if jit_pc outside RAM/ROM range, continue (interpreter recovers)
6. Check spcflags (interrupts, cache invalidation)
7. Look up next block in kpx_cpu block cache, loop
```

**Contract on step 4**: On return from fn(), PPCR_PC is the next PC to execute.
All GPRs, CR, XER, FPR, LR, CTR are architectural.

**Contract on step 5**: PC validation is a containment guard, not an expected code path.
A compiled block that sets PC to an illegal value is a compiler bug.

### Interpreter fallback contract

When `jblk.complete == false`, the dispatch loop calls `goto skip_jit` and the interpreter
executes the block instead. No state from the partial compilation attempt is observable —
the JIT only modifies the code cache, not the register struct, during compilation.

The interpreter starts from the same `pc()` value the JIT would have started from.
The two paths are observationally equivalent for any block that reaches this fallback.

---

## Gates — ownership and intent

### Gate 1: `SS_USE_JIT` environment variable

**Location**: `ppc-cpu.cpp` line ~698

**Classification**: CONTAINMENT — temporary, not a permanent architectural feature.

**What it protects**: Ability to run SheepShaver with interpreter-only execution for debugging and comparison.

**Invariant guarded**: Interpreter/JIT build parity (Invariant 5 from JIT-APPROACH-RESET).

**Expiry condition**: When the JIT is contract-clean and boot-stable, this gate should be removed and JIT should be the default. Until then, it remains as a controlled activation mechanism.

**Replacement**: The JIT should be unconditionally enabled with a proven runtime contract. `SS_USE_JIT` becomes obsolete at that point.

**Proof workload**: boot-to-desktop + Speedometer benchmark green.

---

### Gate 2: `blk.complete` check in ppc-cpu.cpp

**Location**: `ppc-cpu.cpp` line ~702: `if (ppc_jit_aarch64_compile(...) && jblk.complete)`

**Classification**: CONTAINMENT — prevents executing partial blocks natively.

**What it protects**: Correctness of interpreter fallback. A block that was partially compiled has a midpoint PC written to PPCR_PC at the truncation site. The interpreter must execute from that PC. If we allowed partial blocks to run, the truncation epilogue would set PPCR_PC to the first unhandled instruction, then the interpreter would continue from there — this is actually correct behavior. The gate is therefore **conservative**.

**Invariant guarded**: Fault recovery restartability.

**Status for relaxation**: This gate can be removed when we are confident that:
1. The truncation epilogue always writes a valid PPCR_PC
2. The interpreter can safely resume from that PC

That is already true. The gate is overcautious. **This gate is a candidate for removal.**

**Proof workload**: interpreter-only boot and JIT boot must be bit-identical in register state at REGDUMP.

---

### Gate 3: `compile_one` returning false

**Location**: `ppc-jit.cpp` — every unrecognized opcode returns false.

**Classification**: PERMANENT SEMANTIC EXCLUSION for opcodes not yet implemented;
DIAGNOSTIC for opcodes that should be implemented but aren't.

**What it protects**: Prevents executing incorrect code for unimplemented instructions.

**Rule**: Every `return false` path in `compile_one` should have a comment classifying it as:
- `/* UNIMPLEMENTED: [opcode name] — not yet native, interpreter handles */`
- `/* EXCLUDED: [reason] — permanent interpreter delegation */`

Currently most are undocumented. This must be fixed as part of the approach-reset audit.

---

### Gate 4: EMUL_OP class (opcode 6) — no handler in compile_one

**Location**: `ppc-jit.cpp` — opcode 6 has no case in the switch, returns false.

**Classification**: PERMANENT SEMANTIC EXCLUSION — EMUL_OP blocks invoke the Mac OS emulation
layer and are not amenable to native inline codegen without a full helper infrastructure.

**Invariant guarded**: This is the "exact runtime helper + block barrier" pattern from the
JIT-APPROACH-RESET document, applied correctly. EMUL_OP is always handled by the interpreter,
providing a clean block barrier.

**Status**: Correct and permanent. Do not attempt to inline EMUL_OP without a full
barrier-and-helper framework.

---

## Critical architectural gap: no block cache

**Status**: KNOWN CONTRACT VIOLATION — incomplete JIT architecture.

The current JIT has no lookup table from PPC address to compiled code. Every call to
`ppc_jit_aarch64_compile()` recompiles the block from scratch. The cache write pointer
advances linearly; compiled blocks accumulate but are never reused.

**Consequences**:
1. Hot loops are recompiled on every iteration — no performance amortization
2. The 4MB code cache fills up over time, after which all compilation fails
3. The JIT cannot be said to "cache" compiled code in any architectural sense

**Required fix**: A block address cache (PC → compiled_code_ptr) must be added.
The cache should map `uint32_t ppc_pc → uint32_t *code_ptr`. Before compiling, look up the
PC in the cache. If found, execute the cached code directly. If not found, compile and insert.

**Flush discipline**: The cache must be fully flushed on any Mac OS icbi/isync event that
covers the block's address range. `ppc_jit_aarch64_flush()` already handles global flush.

**Gate blocking this**: Until the block cache is implemented, the JIT provides correctness
but not performance. The `SS_USE_JIT` gate prevents it from degrading performance in
production use.

---

## Helper contract

No runtime helpers are called from compiled blocks in the current PPC JIT. All complex
operations (EMUL_OP, unimplemented opcodes) cause block termination and interpreter fallback.

This is architecturally clean: every helper call is a full block barrier by construction.

If helpers are added in the future, they must be classified as H1 (exact + mandatory barrier)
unless explicitly proven to be H2 (continuation allowed after proof of state consistency).

---

## Fault recovery contract

### Scenario: fault in compiled code

If a compiled block faults (e.g., bad memory address from a LDR/STR in compiled code):
1. The host SIGSEGV fires at the ARM64 instruction that faulted.
2. `sigsegv_handler()` in `sheepshaver_glue.cpp` is called.
3. The handler checks `cpu->pc()` (reads PPCR_PC) to determine if we are in a Mac fault context.
4. PPCR_PC = block entry PC (see PC model above).
5. The handler can correctly identify the Mac context and either handle (known ROM faults) or dump+quit.

### Restartability guarantee

A compiled block is restartable from its entry PC. The interpreter will re-execute the block
cleanly from that PC. Because the JIT does not commit partial instruction results (each handler
is atomic from the struct's perspective), there is no partial-commit hazard.

**Rule**: Any future instruction handler that spans multiple struct writes must either:
1. Be atomic from a fault perspective (all writes or none), OR
2. Emit an explicit PC update mid-handler to enable per-write restartability.

---

## PC contract at dispatch loop boundaries

### Before JIT call (`fn(regs_ptr())`)

- `pc()` = `regs.pc` = current block entry PC (correct, set by interpreter's last `increment_pc`)
- All GPRs, CR, XER, LR, CTR = architectural

### After JIT call returns

- `pc()` = `regs.pc` = next PC to execute (set by epilogue)
- All GPRs, CR, XER, FPR, LR, CTR = architectural

### After fallback to interpreter

- `pc()` = same value as before JIT call (JIT compilation does not modify struct)
- All state = unchanged from pre-JIT-call

---

## Opcode classification

All PPC opcodes handled by the JIT are in one of three categories:

### Category A: Full inline codegen

The opcode is fully handled natively. On exit from the handler, all modified architectural
state is materialized. Examples: add, sub, or, and, ld/st, compare, branch.

### Category B: Interpreter delegation (compile_one returns false)

The opcode terminates the block. The interpreter handles it. This is the correct and safe
pattern for all unimplemented or barrier-worthy classes. Examples: EMUL_OP (opcode 6),
unimplemented AltiVec, unimplemented FPU families.

### Category C: (Not yet present) Helper dispatch

Future use: for complex opcodes that can be compiled to a helper call with a mandatory block
barrier. Not implemented in the current JIT.

---

## Invariant summary table

| # | Invariant | SheepShaver PPC JIT status |
|---|-----------|---------------------------|
| 1 | Exactly one authoritative PC at each boundary | ✅ PPCR_PC is the single source of truth. Written at block exit by epilogue. Block entry PC is stale mid-block (see note). |
| 2 | Lazy flags valid only while ownership is unambiguous | ✅ No lazy flags. CR/XER/FPSCR always materialized per-instruction. |
| 3 | Helper calls are semantic barriers | ✅ No helpers in compiled code. All unhandled ops → interpreter (full block barrier). |
| 4 | Block chaining must not bypass validation | ✅ No block chaining. Every block exits through epilogue, dispatcher finds next block. |
| 5 | Interpreter and JIT builds agree on shared semantics | ⚠️ Partially verified via SS_TEST_JIT harness. Full parity audit pending. |
| 6 | Fault recovery: restartable from coherent state | ✅ Block-level restartability. PPCR_PC = block entry on fault. Interpreter re-runs block. |
| 7 | Every exception path chooses exact model or barrier | ✅ EMUL_OP and unhandled opcodes → interpreter delegation (Category B). |

---

## Known weak seams

### Weak seam 1: No block address cache

Compiled code is never reused. The JIT recompiles every block on every execution.
This is a fundamental architectural gap. The block cache must be added before the JIT
can deliver meaningful performance benefit.

### Weak seam 2: `blk.complete` gate is overcautious

Partial blocks are safe to execute — the truncation epilogue writes a valid PPCR_PC and
the interpreter can resume from there. The gate prevents this, causing unnecessary interpreter
fallback for blocks that partially compile. This reduces JIT coverage.

### Weak seam 3: PC validation guard after JIT call may mask compiler bugs

The PC range check after `fn(regs_ptr())` silently skips blocks that set an out-of-range PC.
This can mask JIT compiler bugs. It should log the occurrence rather than silently continuing.

### Weak seam 4: Gate comment drift

Several `return false` paths in `compile_one` lack classification comments. As a result,
the distinction between "not yet implemented" and "permanently excluded" is not visible in code.

### Weak seam 5: FPSCR sync coverage

FPSCR rounding mode is synced on `mtfsfi`/`mtfsf`/`mtfsb0`/`mtfsb1`. It is not verified
that all ARM64 FP instructions correctly observe the rounding mode. This should be audited
systematically against the FPSCR-to-FPCR mapping.

---

## Contributor checklist

Before changing the JIT compiler, dispatch path, or fallback behavior:

1. Which boundary is being changed?
2. Is PPCR_PC authoritative at that boundary after the change?
3. Are CR and XER still materialized before the changed boundary?
4. Can the interpreter resume safely if the JIT path fails at that point?
5. Does this change affect the behavior of interpreter-only builds?
6. Which golden workload proves the change is safe?

If any answer is unclear, the change is not ready.
