# SheepShaver AArch64 JIT — Project Plan

## Goal

Bring SheepShaver's PPC emulation to full native performance on AArch64,
starting with an optimized interpreter and progressing to a direct-codegen JIT.

## Phases

### Phase 1: Optimized Interpreter (weeks 1–4)

Make the existing PPC interpreter fast enough for interactive Mac OS 8/9 use
on AArch64 without any JIT.

| Task | Est. | Files |
|------|------|-------|
| Threaded dispatch via computed goto | 3 days | `ppc-execute.cpp` |
| Hot-path inlining for top-20 PPC ops | 1 week | `ppc-execute.cpp` |
| Direct-addressing memory fast path | 1 week | `ppc-cpu.cpp`, `sheepshaver_glue.cpp` |
| PGO build support (Makefile/configure) | 2 days | `configure.ac`, `Makefile.in` |
| Test harness (PPC opcode equivalence) | 1 week | `jit-test/` |
| Benchmark: baseline vs optimized MIPS | 2 days | `jit-test/bench.sh` |

**Exit criteria:** interpreter passes all opcode tests, boots Mac OS 8.x,
≥2x speedup over unoptimized baseline on AArch64.

### Phase 2: JIT Scaffolding (weeks 5–8)

Build the direct-codegen JIT infrastructure, reusing BasiliskII's ARM64
instruction emitter and block dispatch patterns.

| Task | Est. | Files |
|------|------|-------|
| Port `codegen_arm64.h/cpp` to kpx_cpu | 2 days | `jit/aarch64/codegen_aarch64.h` |
| Block cache + RWX mapping for AArch64 | 1 week | `jit/aarch64/jit-target-cache.hpp` |
| Block dispatch / entry / exit stubs | 1 week | `jit/aarch64/jit-target-dispatch.cpp` |
| PPC register → ARM64 register mapping | 3 days | `jit/aarch64/ppc-regmap-aarch64.h` |
| Compile/execute loop integration | 1 week | `ppc-jit-aarch64.cpp`, `sheepshaver_glue.cpp` |
| configure.ac: `--enable-aarch64-jit` | 1 day | `configure.ac` |

**Exit criteria:** JIT compiles trivial blocks (NOP, branch), falls back to
interpreter for everything else, boots Mac OS with mixed execution.

### Phase 3: Integer Opcode Handlers (weeks 9–16)

Implement native ARM64 codegen for all integer PPC instructions.

| Category | Ops | Est. |
|----------|-----|------|
| Load/store (byte/half/word, indexed) | ~20 | 2 weeks |
| Integer ALU (add/sub/and/or/xor/neg) | ~15 | 1.5 weeks |
| Shift/rotate (slw/srw/sraw/rlwinm/rlwimi) | ~8 | 1.5 weeks |
| Compare (cmp/cmpi/cmpl/cmpli) | ~6 | 1 week |
| Branch (b/bl/bc/bclr/bcctr) + LR/CTR | ~8 | 2 weeks |
| Condition register (crand/cror/mcrf/mcrxr) | ~10 | 1 week |
| System (mfspr/mtspr/mfcr/mtcrf/sc/rfi) | ~8 | 1 week |
| Mul/div (mullw/mulhw/divw/divwu) | ~6 | 1 week |

**Exit criteria:** all integer opcode tests pass, Mac OS boots to desktop
with JIT handling >90% of executed instructions.

### Phase 4: FPU (weeks 17–24)

Implement PPC floating-point → ARM64 NEON/FP codegen.

| Category | Ops | Est. |
|----------|-----|------|
| FP load/store (lfs/lfd/stfs/stfd) | ~8 | 1.5 weeks |
| FP arithmetic (fadd/fsub/fmul/fdiv/fmadd) | ~12 | 2 weeks |
| FP compare (fcmpu/fcmpo) → CR | ~4 | 1 week |
| FP convert (frsp/fctiw/fctiwz/fctid) | ~6 | 1 week |
| FP move (fmr/fneg/fabs/fnabs) | ~4 | 3 days |
| FP rounding mode (mtfsfi/mtfsf/mffs/mtfsb) | ~6 | 1 week |
| IEEE 754 edge cases + NaN handling | — | 1.5 weeks |

**Exit criteria:** FPU opcode tests pass, FP-heavy apps work correctly.

### Phase 5: Optimization + Polish (weeks 25–30)

| Task | Est. |
|------|------|
| Block-level optimizations (constant prop, dead code) | 2 weeks |
| Register allocation improvements | 1.5 weeks |
| Hot-loop detection + unrolling | 1 week |
| Performance benchmarking + profiling | 1 week |
| Boot smoke tests across Mac OS 7.6–9.0.4 | 1 week |

## Architecture

### Interpreter (Phase 1)
```
PPC instruction → ppc-decode.cpp → ppc-execute.cpp (computed goto dispatch)
                                    ↓
                            direct memory access via MEMBaseDiff
```

### JIT (Phases 2–4)
```
PPC instruction → ppc-decode.cpp → ppc-translate-aarch64.cpp (select handler)
                                    ↓
                            ppc-codegen-aarch64.cpp (emit ARM64 instructions)
                                    ↓
                            codegen_aarch64.h (ARM64 instruction encoding)
                                    ↓
                            jit-cache (RWX block, icache flush)
                                    ↓
                            block dispatch (direct chaining, popallspace)
```

### Reused from BasiliskII
- `codegen_arm64.h` — ARM64 instruction encoding macros
- Block dispatch patterns (popallspace, execute_normal, block chaining)
- Flag handling patterns (NZCV ↔ PPC CR mapping)
- Test harness approach (B2_TEST_HEX / REGDUMP)

### PPC register mapping (tentative)
```
PPC GPR[0-7]   → ARM64 x19-x26 (callee-saved, hot registers)
PPC GPR[8-31]  → memory (regfile array)
PPC LR         → ARM64 x27
PPC CTR        → ARM64 x28
PPC CR         → memory (8 × 4-bit fields)
PPC XER        → memory
PPC FPR[0-7]   → ARM64 d8-d15 (callee-saved)
PPC FPR[8-31]  → memory (FP regfile array)
CPU state ptr  → ARM64 x20 (always points to powerpc_cpu state)
```

## Test Strategy

Same approach as BasiliskII's jit-test harness:
- Each test = hex-encoded PPC instruction sequence
- Run under interpreter and JIT
- Compare full register dump (GPR0-31, CR, LR, CTR, XER, FPSCR)
- Deterministic, bounded, no ROM dependency

## Constraints

- No dyngen — direct ARM64 emission only
- No ROM patches to work around JIT bugs
- Test-driven: every opcode handler verified before moving on
- Interpreter always available as fallback
