# BasiliskII AArch64 JIT — Status

## Current State (2026-04-18)

**Build:** ✅
**Interpreter:** ✅ Boots Mac OS 7.x, runs idle loop
**JIT optlev=0:** ✅ Full boot, 9.5B instructions, zero SEGVs, idle loop reached
**JIT optlev=2:** ⚠️ SEGVs during early init from block handoff bug for barrier instructions

## Test Harness

**378 total vectors, 227 risky active, score=100**

Complete 68K instruction set coverage:

| Category | Opcodes Tested |
|----------|---------------|
| Data movement | MOVE (B/W/L), MOVEA, MOVEQ, MOVEM (predec/postinc/mixed), MOVEP (W/L), MOVE16, LEA, PEA, EXG, SWAP, LINK/UNLK |
| Arithmetic | ADD/SUB/CMP (B/W/L + imm + quick + addr), ADDA/SUBA/CMPA, ADDX/SUBX, NEG/NEGX, CLR, MUL (U/S/L), DIV (U/S/L) |
| Logic | AND/OR/EOR/NOT (B/W/L + imm + mem), TST |
| Shift/Rotate reg | ASL/ASR/LSL/LSR/ROL/ROR (all sizes, imm + reg count) |
| Shift/Rotate mem | ASLW/ASRW/LSLW/LSRW/ROLW/RORW/ROXLW/ROXRW |
| ROXL/ROXR | X-flag propagation, all counts (0/1/2/32/33/63), B/W/L sizes |
| Bit ops | BTST/BSET/BCLR/BCHG (reg + mem + imm), high-bit tests |
| Bit fields | BFEXTU/BFEXTS/BFFFO/BFSET/BFCLR/BFCHG/BFTST/BFINS |
| BCD | ABCD/SBCD/NBCD (basic + carry/borrow), PACK/UNPK |
| Branch | Bcc (16 conditions × taken/not-taken × short/word), chained branches |
| BSR/JSR | BSR.B/W/L, JSR (An)/(d16,PC), nested BSR, dispatch loop pattern |
| DBcc | DBRA/DBF/DBEQ/DBMI/etc, 1-1000 iteration loops, terminal/wrap |
| Scc | All Scc families + CCR-preservation interactions |
| SR/CCR | MOVE to/from SR, ORI/ANDI/EORI to SR/CCR, RTR |
| Control regs | MOVEC (CACR/VBR/TC/DTT0/DTT1), MOVES, MVR2USP/MVUSP2R |
| Cache | CINVA, CPUSHA, full 68040 cache init sequence |
| Memory ops | Store/load roundtrip, indexed (d8,An,Xn) with scale, PEA+MOVEM stack |
| Addressing | Dn, An, (An), (An)+, -(An), (d16,An), (d8,An,Xn), (d16,PC), (xxx).W, (xxx).L, #imm, odd address, A7 byte adjust |
| Multi-block | Flag propagation across JSR/RTS, MOVEM save/restore, trap dispatch pattern |

## JIT Opcode Compilation

### Natively compiled (optlev=2)
Bcc, DBcc, Scc, BSR, JSR, JMP, RTS, RTD, LINK, UNLK, LEA, PEA, MOVE, MOVEA, ADD, SUB, AND, OR, EOR, CMP, CMPA, CMPM, ADDA, SUBA, ADDX, SUBX, NEG, NEGX, NOT, CLR, TST, EXT, SWAP, EXG, BTST, BSET, BCLR, BCHG, LSL, LSR, ASL, ASR, ROL, ROR, MULU, MULS, MOVEM, NOP, FPP, FBcc, FScc, MOVE16, bit field ops (BFTST/BFSET/BFCLR/BFCHG/BFEXTU/BFEXTS/BFFFO), CINVL/CINVP/CPUSHL/CPUSHP

### Interpreter fallback (58 opcodes)
SR ops (MV2SR, MVSR2, ORSR, ANDSR, EORSR), RTE/RTR, MOVEC2/MOVE2C, DIVS/DIVU/MULL/DIVL, ROXL/ROXR, memory shifts (8 variants), TAS, CHK/CHK2, TRAP/TRAPcc/TRAPV, ABCD/SBCD/NBCD/PACK/UNPK, BFINS, CAS/CAS2, MOVES, MOVEP, CINVA/CPUSHA, FSAVE/FRESTORE/FDBcc/FTRAPcc, STOP/RESET, MVR2USP/MVUSP2R

All 58 fallback opcodes verified correct through the test harness.

## Key Bugs Fixed

1. **DBRA loop unrolling** — block tracer followed backward branches, baking loop iterations into single blocks
2. **DBcc CCR leakage (case 1 + cases 2-15)** — `live_flags()` set `flags_on_stack=TRASH`; fixed with `discard_flags_in_nzcv()` + `save_and_discard_flags_in_nzcv()` + `dbcc_cond_move_ne_w()` midfunc
3. **X flag preservation in DBcc** — raw ARM64 MRS+AND+STR bypassing register allocator
4. **flags_to_stack carry inversion** — carry flip on early-return path
5. **Block-entry NZCV reload** — LDR+MSR at every compiled block start
6. **Trap dispatch magic cookie** — `$0DB0` written via EMULOP at ROM+0x2726
7. **Handler pointer** — `$0120` set to ROMBaseMac + 0x280E
8. **ROMBase global** — `$02AE` written in PatchROM
9. **FPU save/restore crash** — RTS patch for FSAVE/FRESTORE when FPU disabled
10. **24-bit PC sign extension** — recovery in execute_normal
11. **SIGBUS on Linux/aarch64** — added to signal handler
12. **B2_JIT_FORCE_TRANSLATE** — forces optcount[0]=0 for test harness

## Architecture

### Block Compilation Pipeline
1. Trace loop: interpreter builds `pc_hist[]`, forward branches inlined, backward branches end block
2. compile_block: block-entry NZCV reload, then per-instruction native code generation
3. Mid-block side exits for branches
4. Tick injection every 64 instructions
5. Final endblock with countdown + block chaining

### Key Infrastructure
- `discard_flags_in_nzcv()`: evicts FLAGTMP, sets flags_on_stack=VALID
- `save_and_discard_flags_in_nzcv()`: saves NZCV+X via raw ARM64, then discards
- `dbcc_cond_move_ne_w()`: CBZ-based conditional move (no NZCV clobber)
- `B2_JIT_FORCE_TRANSLATE`: immediate native compilation for tests
- `B2_JIT_VERIFY_BLOCKS`: interpreter-vs-JIT block comparison
- `FIX_DISPATCH_MAGIC` EMULOP: writes $0DB0, $0120, $02AE, saves SR

### gencomp.c is the active code generator
Generates `compemu.cpp` with opcode handlers. `gencomp_arm.c` exists but is NOT used on AArch64.

## Test Environment
- ROM: Quadra800.ROM (1MB, version 1660)
- Disk: HD200MB (Mac OS 7.x)
- Display: Xvfb :99, SDL software renderer, 640×480
- Config: `jit true`, `jitcachesize 8192`, `modelid 14`, `cpu 4`, `fpu false`
