# BasiliskII AArch64 JIT — Bringup Status

## Current State (2026-04-16, evening)

**Build:** ✅
**Interpreter:** ✅ Boots Mac OS 7.x desktop in ~45s
**JIT:** ⚠️ ROM boot progresses past InitAll, System 7.5 loads from disk, but stuck in A-line trap dispatch loop.

**Opcode test harness:** `jit-test/run.sh` — 226+ deterministic vectors, 28 risky vectors active.
- With `B2_JIT_FORCE_TRANSLATE=1` (real native ARM64 compilation): **pass=28 fail=0 score=100**
- All risky vectors pass — registers AND SR flags match interpreter.

## Commit Log (recent, newest first)

| Commit | Description |
|--------|-------------|
| `21dfcdd4` | **Fix DBRA CCR leakage** — set flags_on_stack=VALID in discard_flags_in_nzcv |
| `d305fd2b` | Block-entry NZCV reload + quit_program guard in execute_normal/do_nothing |
| `72ec6291` | Rework DBRA case 1 to avoid clobbering regflags.nzcv (test-before-decrement + `discard_flags_in_nzcv`) |
| `467f3f86` | Update JIT-STATUS.md |
| `8b0e61b0` | **Fix DBRA loop unrolling bug** — block tracer no longer follows backward branches |
| `f9c57cca` | Fix flags_to_stack carry inversion when flags_on_stack already VALID |
| `48418892` | Add dbra_loop_100 vector exposing DBRA multi-iteration bug |
| `6e4bf1fe` | Add BSR/RTS, LINK/UNLK, indexed addressing, byte post-inc, CMPM, MOVE SR vectors |
| `4147942d` | Add memory-indirect, flag-chain, MOVEM, ABCD/NBCD, register-pressure vectors |
| `a4a0bc60` | **Add B2_JIT_FORCE_TRANSLATE** + 21 high-risk opcode vectors |

## Key Bugs Found & Fixed

### 1. DBRA loop unrolling (CRITICAL — fixed)
**Block tracer** (`compemu_legacy_arm64_compat.cpp`): followed backward branches including DBRA loop-back edges during initial trace, unrolling loop iterations into single compiled blocks. A DBRA loop with count=99 would get 16 iterations baked into one block (blocklen=33), so the loop always executed exactly 33 iterations regardless of the runtime counter value.

**Root cause:** Branch-following condition `new_pcp >= blk_start` allowed backward branches within 512 bytes of block start. The target was technically "ahead" of the block start but behind the current instruction — a loop.

**Fix:** Changed to `new_pcp > cur_insn` — only forward branches from the current instruction are inlined. Backward branches end the block and use block chaining.

### 2. DBRA case 1 CCR preservation (reworked, partially fixed)
**M68K spec:** DBcc does NOT affect CCR. The original `sub_w_ri` approach used ARM64 SUBS which clobbered hardware NZCV and triggered `clobber_flags()` → stale writes to `regflags.nzcv`.

**Rework:** Replaced `sub_w_ri` with test-before-decrement: `test_w_rr(a,b)` with a≠b (compat path, no clobber_flags) → `lea_l_brr` + `mov_w_rr` (non-flag-affecting decrement) → `register_branch(NE)`. Added `discard_flags_in_nzcv()` midfunc that evicts FLAGTMP from the register allocator.

**Status:** Register values (D0–D7, A0–A6) are now fully correct. SR still shows stale Z flag from the internal TST through an unidentified write path in the compiled code. Investigation ongoing.

### 3. flags_to_stack carry inversion (fixed)
When `flags_to_stack()` took the early-return path (flags already on stack), it cleared `flags_carry_inverted` without flipping the hardware NZCV carry bit. Any subsequent `jcc` testing hardware NZCV would see wrong carry polarity.

**Fix:** Emit MRS/EOR/MSR carry flip even on the early-return path when carry is inverted.

### 4. B2_JIT_FORCE_TRANSLATE (new feature)
Test harness vectors only execute once, but the JIT requires 10 warm-up executions before promoting to native ARM64 compilation (optlev=2). Without forcing, all tests ran at optlev=0/1 (interpreter fallback) and never exercised real native code.

**Fix:** `B2_JIT_FORCE_TRANSLATE=1` env var sets `optcount[0]=0` in `compemu_support_arm.cpp`, forcing immediate native compilation.

### 5. Block-entry NZCV reload (defensive)
Emit LDR+MSR at every compiled block entry to reload hardware NZCV from `regflags.nzcv`. Ensures correct flag state at block start regardless of what the previous block left in hardware NZCV. Cost: one LDR+MSR per block entry (negligible).

### 6. quit_program guard in execute_normal/do_nothing (defensive)
After M68K_EXEC_RETURN sets `quit_program=1`, the JIT's barrier path exits to `execute_normal()` which could re-enter the interpreter trace loop and execute random instructions past the test boundary. Added early return when `quit_program > 0`.

### 7–11. Earlier fixes (DBF branch inversion, spcflags, side-exit, ISP/MSP, bus error)
See git history for details.

## Known Remaining Issues

### DBRA CCR leakage (FIXED)
JIT's compiled DBRA block writes stale Z flag (`0x40000000`) to `regflags.nzcv` through an unidentified path. The write occurs during compiled block execution (confirmed via 0xDEADBEEF marker). The compat `test_w_rr` function modifies hardware NZCV at runtime, and something propagates it to `regflags.nzcv` despite the compile-time flag state machine indicating no save should occur (`flags_on_stack=VALID`, `flags_in_flags=TRASH`, `FLAGTMP=INMEM`).


**Fixed:** `discard_flags_in_nzcv()` now sets `flags_on_stack=VALID`, forcing the early-return path in `flags_to_stack()` and preserving the correct pre-DBRA flags in `regflags.nzcv`.
- Alternatively, disassemble the compiled DBRA block's native code and trace through it manually

### A-line trap dispatch loop (boot)
ROM+0x26a0 trap handler tight loop. Root cause likely a JIT opcode producing wrong flags/value in the dispatch path. Not being investigated until opcode harness coverage is complete.

## Test Harness (`jit-test/`)

### Design
- Inline hex bytecode vectors (no separate `.s` files)
- Each vector injected via `B2_TEST_HEX`, run in both interpreter and JIT mode
- `REGDUMP:` output (D0–D7, A0–A6, SR) diffed between modes
- `B2_JIT_FORCE_TRANSLATE=1` forces native ARM64 compilation in JIT mode
- Sentinel `MOVEA.L #imm,A6` verifies test code actually executed
- `active-risky-tests.txt` controls which vectors are in the scored set

### Coverage (226+ vectors total, 28 risky active)
- **Data movement:** MOVE, MOVEQ, MOVEM (predec/postinc/mixed), MOVEA sign-extension
- **ALU:** ADD, SUB, ADDI/SUBI (byte/word/long + wraps), ADDQ/SUBQ, NEG, CLR, NOT, EXT
- **Shift/rotate:** LSL/LSR/ASL/ASR (all sizes, count from reg), ROL/ROR (word), ROXL/ROXR (X propagation, various counts including 0/32/33/63)
- **Bit manipulation:** BTST/BSET/BCLR/BCHG (register + memory, high-bit, immediate)
- **Branch:** All Bcc conditions × taken/not-taken × short/word displacement, branch chains
- **DBcc:** DBRA iterations (1–6, 100), DBF terminal, DBcc condition variants, CCR preservation
- **Scc:** All Scc families + CCR preservation interactions
- **Compare:** CMP, CMPI (all sizes, boundaries), CMPM, TST
- **MUL/DIV:** MULU (large), MULS (neg×neg, zero), DIVU (exact, remainder), DIVS (neg/neg, overflow)
- **BCD:** ABCD (basic + carry), SBCD (basic + borrow), NBCD
- **Extended:** ADDX (basic + chain + Z-flag preservation), SUBX, NEGX (with/without X, multi-precision chain)
- **Flags:** ORI/ANDI/EORI to CCR, MOVE to/from SR, flag propagation chains (X/Z/N interaction)
- **Memory:** Store/load roundtrip, indexed (d8,An,Xn), byte post-increment, push/pop, PEA, ORI to memory
- **Control flow:** BSR/RTS (basic + nested), JSR indirect, LINK/UNLK, DBRA loops (multi-block)
- **Misc:** EXG, SWAP, TAS, NOP, CHK

### Metrics contract
Always emits: `build_ok`, `pass`, `fail`, `total`, `score`, `infra_fail`, `fail_equiv`, `risky_*`

## Architecture

### Block Compilation Pipeline
1. **Trace loop** (`compemu_legacy_arm64_compat.cpp`): interpreter executes M68K building `pc_hist[]`. Forward branches within ±512 bytes continue tracing; backward branches end the block.
2. **compile_block** (`compemu_support_arm.cpp`): emits block-entry NZCV reload, then for each `pc_hist` entry, calls generated opcode handler.
3. **Mid-block side exits**: `B.cond` guard → exit stub → `endblock_pc_inreg`.
4. **Tick injection**: every 64 compiled instructions, `cpu_do_check_ticks()`.
5. **Final endblock**: `compemu_raw_jcc_l_oponly` + predicted/not-predicted paths with countdown and chaining.

### Important: gencomp.c is the active generator
`gencomp.c` generates `compemu.cpp`, NOT `gencomp_arm.c`. The ARM-specific `compemu_support_arm.cpp` provides dispatch infrastructure.

### New infrastructure
- `discard_flags_in_nzcv()` midfunc: evicts FLAGTMP from register allocator without writing to memory
- `B2_JIT_FORCE_TRANSLATE` env var: forces optcount[0]=0 for immediate native compilation
- Block-entry NZCV reload: LDR+MSR from regflags.nzcv at every compiled block start
- `quit_program` guards in execute_normal/do_nothing

## Test Environment
- ROM: Quadra800.ROM (1MB, version 1660)
- Disk: HD200MB (Mac OS 7.x)
- Display: Xvfb :99, SDL software renderer, 640×480
- Config: `jit true`, `jitcachesize 8192`, `modelid 14`, `cpu 4`, `fpu false`

## Boot Investigation (2026-04-16 evening)

### Stuck point
`pc=0x0080279C` — A-line trap dispatch loop. Handler at `0x80280E` reads trap word from `$0AF0` (value 0x000C), dispatches through `JMP (d8,PC,A0.L)` to `0x802310`, returns with D0=0x0C (non-zero error code → retry loop).

### Interpreter comparison
Interpreter boots fully to user mode (`SR=0x2000`) and executes deep into ROM initialization. Never gets stuck at `0x279C`.

### Root cause analysis
- Individual opcodes in the dispatch handler (MOVE.W #imm,SR, MOVE.W (abs).W, MOVEA.L, BNE) all work correctly in JIT test vectors
- The MOVE to SR barrier + fresh block compilation + MOVE.W flags → BNE pattern works correctly in isolation
- The bug is likely in:
  1. The `JMP (d8,PC,A0.L)` complex jump at `0x802826` (LEA $FFFFFAF0,A0 + JMP using PC-relative + scaled index)
  2. The subroutine at `0x802310` or its return path through `JMP (A6)` at `0x804182`
  3. Multi-block state propagation across the JSR→handler→JMP(A6) call chain
  4. Interaction between the trap dispatch and interrupt/timer handling
