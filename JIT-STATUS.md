# MacEmu AArch64 JIT — Status

## SheepShaver PPC JIT (2026-04-18)

**Build:** ✅
**Interpreter:** ✅ Boots Mac OS to desktop (VNC port 5999, ~167 MIPS)
**JIT boot:** ✅ Boots to "Welcome to Mac OS" splash screen with `SS_USE_JIT=1`
**JIT harness:** ✅ 209/209 opcode vectors pass (score=100)
**ROM harness:** ✅ 663/766 ROM blocks pass (86.6%)

### JIT Boot Status

With `SS_USE_JIT=1`, SheepShaver boots Mac OS to the Welcome splash screen.
The boot is slower than pure interpreter mode because:
1. No block cache — each PC re-compiles (O(n) compile overhead per block execution)
2. 62% block completion rate — 38% of blocks fall back to interpreter
3. Cache fills up (4MB), then ALL new blocks fall back to interpreter

The process stays alive ~190s before an interpreter-side SIGSEGV on unmapped
memory (ea=0xae9b, Mac PC outside ROM/RAM). Not a JIT bug.

### ROM Harness

Standalone headless tool: `SheepShaver/rom-harness/`

Loads the Mac ROM, scans for PPC basic blocks, JIT-compiles each,
compares against a built-in reference interpreter. No display, no
hardware, no SheepShaver runtime dependencies.

**Score: 663/766 (86.6%)** on PowerMac 9500 OldWorld ROM.

Remaining failures:
- XER[CA] carry mismatches (29 blocks) — carry propagation bugs
- CR field mismatches (61 blocks) — likely complex multi-field updates
- CTR/PC mismatches (37/31 blocks) — complex bc variants

### Bugs Found & Fixed By ROM Harness

1. **CR logical NOP-default** — mcrf, crand, cror, crxor, crnor, crandc,
   creqv, crorc, crnand silently treated as NOPs (opcode-19 default: return true)
2. **Missing XER[SO] in comparisons** — cmp/cmpi/cmpli/cmpl + emit_update_cr0
   didn't copy XER[SO] to CR field SO bit
3. **Wrong NZCV→CR mapping in cmpi** — raw ARM64 NZCV>>28 used instead of
   proper signed LT/GT/EQ with CSEL
4. **bdz not implemented** — only bdnz handled; bdz (BO=0b01111) misidentified
   as conditional branch
5. **bc epilogue skip-over** — CBZ/CBNZ +8 only skipped 2 instructions, not the
   full ~9 instruction epilogue. Fixed with placeholder-and-patch pattern.

### JIT Opcode Coverage (PPC)

**Fully compiled:** addi, addis, ori, oris, xori, xoris, andi., andis.,
mulli, subfic, addic, addic., cmpi, cmpli, rlwinm, rlwnm, rlwimi,
add, addc, adde, addme, addze, subf, subfc, subfe, subfme, subfze, neg,
mullw, mulhw, mulhwu, divw, divwu, and, andc, or, nor, xor, eqv, orc, nand,
slw, srw, sraw, srawi, cntlzw, extsh, extsb, cmp, cmpl, mfspr, mtspr, mfcr,
mtcrf, b, bl, bc (bdnz/bdz/beq/bne/blt/bgt/ble/bge), bclr, bcctr, blr, bctr,
mcrf, crand, cror, crxor, crnor, crandc, creqv, crorc, crnand,
lwz, lwzu, lbz, lbzu, lhz, lhzu, lha, lhau,
stw, stwu, stb, stbu, sth, sthu, stwcx., lwarx,
lwzx, lbzx, lhzx, stwx, stbx, sthx, lwbrx, sthbrx,
fadd, fsub, fmul, fdiv, fmadd, fmsub, fnmadd, fnmsub, fabs, fneg, fnabs,
fmr, fctiw, fctiwz, frsp, fcmpu, mffs, mtfsf, mtfsfi, mtfsb0, mtfsb1,
lfs, lfd, stfs, stfd, lfsx, lfdx, stfsx, stfdx,
lswi, stswi, isync, sc, AltiVec (vadd, vsub, vand, vor, vxor, etc.)

**Fallback to interpreter:** opcode 0 (illegal), opc 1 (mulli variants), opc 5,
opc 9 (power), opc 16 (complex bc: decrement+condition combo), opc 22,
opc 29, opc 56, opc 60, opc 61, opc 63 (some FP), XO31:538

---

## BasiliskII 68K JIT

**Build:** ✅
**Interpreter:** ✅ Boots Mac OS 7.x, idle loop reached
**JIT optlev=0:** ✅ Full boot, 9.5B instructions, zero SEGVs
**JIT optlev=2:** ⚠️ SEGVs during early init from block handoff bug
**JIT harness:** 26/28 vectors pass (2 SR-only flag mismatches)

See `BasiliskII/src/uae_cpu_2026/compiler/` for the 68K → AArch64 JIT.

Full 68K opcode test coverage documented below.

### Test Harness (68K)

**378 total vectors, 227 risky active, score=100**

| Category | Opcodes Tested |
|----------|---------------|
| Data movement | MOVE (B/W/L), MOVEA, MOVEQ, MOVEM, MOVEP, MOVE16, LEA, PEA, EXG, SWAP, LINK/UNLK |
| Arithmetic | ADD/SUB/CMP (B/W/L + imm + quick + addr), ADDA/SUBA/CMPA, ADDX/SUBX, NEG/NEGX, CLR, MUL, DIV |
| Logic | AND/OR/EOR/NOT, TST |
| Shift/Rotate | ASL/ASR/LSL/LSR/ROL/ROR/ROXL/ROXR (all sizes, all variants) |
| Bit ops | BTST/BSET/BCLR/BCHG, bit fields (BFTST-BFINS) |
| BCD | ABCD/SBCD/NBCD, PACK/UNPK |
| Branch | Bcc, BSR/JSR, DBcc, Scc |
| SR/CCR | MOVE to/from SR, ORI/ANDI/EORI to SR/CCR, RTR |
| Control | MOVEC, MOVES, CINVA, CPUSHA |
