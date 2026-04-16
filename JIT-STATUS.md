# BasiliskII AArch64 JIT — Bringup Status

## Current State (2026-04-16)

**Build:** ✅
**Interpreter:** ✅ Boots Mac OS 7.x desktop in ~45s
**JIT:** ⚠️ ROM boot progresses past InitAll, System 7.5 loads from disk, but stuck in A-line trap dispatch loop.

**Opcode test harness:** `jit-test/run.sh` — 226 deterministic vectors, 28 risky vectors active.
- With `B2_JIT_FORCE_TRANSLATE=1` (real native ARM64 compilation): **pass=26 fail=2 score=92**
- Both failures are SR-only mismatches (DBRA CCR leakage); all register values correct.

## Commit Log (recent)

| Commit | Description |
|--------|-------------|
| `8b0e61b0` | **Fix DBRA loop unrolling bug** — block tracer no longer follows backward branches |
| `f9c57cca` | Fix flags_to_stack carry inversion when flags_on_stack already VALID |
| `48418892` | Add dbra_loop_100 vector exposing DBRA multi-iteration bug |
| `6e4bf1fe` | Add BSR/RTS, LINK/UNLK, indexed addressing, byte post-inc, CMPM, MOVE SR vectors |
| `4147942d` | Add memory-indirect, flag-chain, MOVEM, ABCD/NBCD, register-pressure vectors |
| `a4a0bc60` | **Add B2_JIT_FORCE_TRANSLATE** + 21 high-risk opcode vectors |
| `76282e13` | Add nop-triplet vector and strict TESTS word-format preflight |
| `a378670c` | Harvest expanded deterministic opcode-equivalence vectors (53 total) |
| `eb216bd9` | Harvest autoresearch harness hardening and expanded deterministic vectors |

## Key Bugs Found & Fixed

### 1. DBRA loop unrolling (CRITICAL — fixed 2026-04-16)
**Block tracer** (`compemu_legacy_arm64_compat.cpp`): followed backward branches including DBRA loop-back edges during initial trace, unrolling loop iterations into single compiled blocks. A DBRA loop with count=99 would get 16 iterations baked into one block (blocklen=33), so the loop always executed exactly 33 iterations regardless of the runtime counter value.

**Root cause:** Branch-following condition `new_pcp >= blk_start` allowed backward branches within 512 bytes of block start. The target was technically "ahead" of the block start but behind the current instruction — a loop.

**Fix:** Changed to `new_pcp > cur_insn` — only forward branches from the current instruction are inlined. Backward branches end the block and use block chaining.

### 2. flags_to_stack carry inversion (fixed 2026-04-16)
When `flags_to_stack()` took the early-return path (flags already on stack), it cleared `flags_carry_inverted` without emitting the carry flip in hardware NZCV. Any subsequent `jcc` testing hardware NZCV would see wrong carry polarity.

**Fix:** Emit MRS/EOR/MSR carry flip even on the early-return path when carry is inverted and flags are valid in hardware.

### 3. B2_JIT_FORCE_TRANSLATE (new feature — 2026-04-16)
Test harness vectors only execute once, but the JIT requires 10 warm-up executions before promoting to native ARM64 compilation (optlev=2). Without forcing, all tests ran at optlev=0/1 (interpreter fallback) and never exercised real native code.

**Fix:** `B2_JIT_FORCE_TRANSLATE=1` env var sets `optcount[0]=0` in `compemu_support_arm.cpp`, forcing immediate native compilation.

### 4. DBF branch condition inverted (fixed earlier)
`register_branch(v1,v2,NATIVE_CC_CC)` → `NATIVE_CC_CS`. ARM64 SUB carry polarity is opposite of x86.

### 5. spcflags perpetually set (fixed earlier)
60Hz timer thread set `SPCFLAG_INT` every 16ms, defeating direct block chaining.

### 6–8. Side-exit REG_PC_TMP, ISP/MSP sync, bus error for unmapped PC (fixed earlier)

## Known Remaining Issues

### DBRA CCR leakage (minor)
JIT's compiled DBRA uses `sub_w_ri` which modifies hardware NZCV. The M68K spec says DBcc does NOT affect CCR. The internal decrement flags (C=1, N=1 on terminal 0→0xFFFF) leak to the final SR. Register values are correct; only CCR bits differ.

**Impact:** 2 test vectors fail (dbra_three_iter, dbra_loop_100) on SR comparison only.

### A-line trap dispatch loop (boot)
ROM+0x26a0 trap handler tight loop. Root cause likely a JIT opcode producing wrong flags/value in the dispatch path. Not being investigated until opcode harness coverage is complete.

## Test Harness (`jit-test/`)

### Design
- Inline hex bytecode vectors (no separate `.s` files)
- Each vector injected via `B2_TEST_HEX`, run in both interpreter and JIT mode
- `REGDUMP:` output (D0–D7, A0–A6, SR) diffed between modes
- `B2_JIT_FORCE_TRANSLATE=1` forces native ARM64 compilation in JIT mode
- Sentinel `MOVEA.L #imm,A6` verifies test code actually executed

### Coverage (226 vectors total, 28 risky active)
- **Data movement:** MOVE, MOVEQ, MOVEM (predec/postinc), MOVEA sign-extension
- **ALU:** ADD, SUB, ADDI/SUBI (byte/word/long + wraps), ADDQ/SUBQ, NEG, CLR, NOT, EXT
- **Shift/rotate:** LSL/LSR/ASL/ASR (all sizes, count from reg), ROL/ROR (word), ROXL/ROXR (X propagation, various counts)
- **Bit manipulation:** BTST/BSET/BCLR/BCHG (register + memory, high-bit)
- **Branch:** All Bcc conditions × taken/not-taken × short/word displacement, branch chains
- **DBcc:** DBRA iterations (1–6, 100), DBF terminal, DBcc condition variants, CCR preservation
- **Scc:** All Scc families + CCR preservation interactions
- **Compare:** CMP, CMPI (all sizes, boundaries), CMPM, TST
- **MUL/DIV:** MULU, MULS (neg×neg, zero), DIVU (exact, remainder), DIVS (neg/neg, overflow)
- **BCD:** ABCD, SBCD (basic + carry/borrow), NBCD
- **Extended:** ADDX, SUBX, NEGX (with/without X, multi-precision chain)
- **Flags:** ORI/ANDI/EORI to CCR, MOVE to/from SR, flag propagation chains
- **Memory:** Store/load roundtrip, indexed (d8,An,Xn), byte post-increment, push/pop, PEA
- **Control flow:** BSR/RTS, nested BSR, JSR indirect, LINK/UNLK
- **Misc:** EXG, SWAP, TAS, NOP, CHK

### Metrics contract
Always emits: `build_ok`, `pass`, `fail`, `total`, `score`, `infra_fail`, `fail_equiv`

## Architecture

### Block Compilation Pipeline
1. **Trace loop** (`compemu_legacy_arm64_compat.cpp`): interpreter executes M68K building `pc_hist[]`. Forward branches within ±512 bytes continue tracing; backward branches end the block.
2. **compile_block** (`compemu_support_arm.cpp`): for each `pc_hist` entry, calls generated opcode handler.
3. **Mid-block side exits**: `B.cond` guard → exit stub → `endblock_pc_inreg`.
4. **Tick injection**: every 16 compiled instructions, `cpu_do_check_ticks()`.
5. **Final endblock**: `compemu_raw_jcc_l_oponly` + predicted/not-predicted paths.

### Important: gencomp.c is the active generator
`gencomp.c` generates `compemu.cpp`, NOT `gencomp_arm.c`. The ARM-specific `compemu_support_arm.cpp` provides dispatch infrastructure.

## Test Environment
- ROM: Quadra800.ROM (1MB, version 1660)
- Disk: HD200MB (Mac OS 7.x)
- Display: Xvfb :99, SDL software renderer, 640×480
- Config: `jit true`, `jitcachesize 8192`, `modelid 14`, `cpu 4`, `fpu false`
