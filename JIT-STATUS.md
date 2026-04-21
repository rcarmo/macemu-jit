# MacEmu AArch64 JIT — Status

## SheepShaver PPC JIT (2026-04-21)

**Build:** ✅
**Interpreter:** ✅ Boots Mac OS to desktop (VNC port 5999, ~167 MIPS)
**JIT boot:** ✅ Boots to "Welcome to Mac OS" splash screen with `SS_USE_JIT=1`
**JIT harness:** ✅ 209/209 opcode vectors pass (score=100)
**ROM harness:** ✅ 1800/1825 ROM blocks pass (98.6%) on 10K-block scan

### JIT Boot Status

With `SS_USE_JIT=1`, SheepShaver boots Mac OS to the Welcome splash screen.
The boot is slower than pure interpreter mode because:
1. No block cache — each PC re-compiles (O(n) compile overhead per block execution)
2. ~71% block completion rate — rest fall back to interpreter
3. Cache fills up (4MB), then ALL new blocks fall back to interpreter

### ROM Harness

Standalone headless tool: `SheepShaver/rom-harness/`

Loads the Mac ROM, scans for PPC basic blocks, JIT-compiles each,
compares against a built-in reference interpreter. No display, no
hardware, no SheepShaver runtime dependencies.

**Score: 1800/1825 (98.6%)** on PowerMac 9500 OldWorld ROM (10K-block scan).

Remaining 25 failures: CR field interactions in multi-instruction blocks
and complex branch BO patterns (CTR+condition combo).

### VNC Input

VNC keyboard and mouse work for remote control:
- Keyboard: SDL event queue drain → ADB key injection
- Mouse: direct ADB injection from VNC server thread (bypasses SDL for reliability)
- Port 5999 (configurable via `vncport` pref)

### Opcode Census — 285 Unique PPC Opcodes Inlined as ARM64

| Category | Count | Status |
|----------|-------|--------|
| Integer ALU (immediate) | 12 | ✅ addi/lis/addic/subfic/mulli/ori/oris/xori/xoris/andi./andis. |
| Integer ALU (register) | 15 | ✅ add/addc/adde/addme/addze/subf/subfc/subfe/subfme/subfze/neg/mullw/mulhw/divw |
| Logical (register) | 10 | ✅ and/andc/or/nor/xor/eqv/orc/nand/slw/srw |
| Shift/Rotate | 6 | ✅ sraw/srawi/rlwinm/rlwimi/rlwnm/cntlzw |
| Sign extend | 2 | ✅ extsh/extsb |
| Compare | 4 | ✅ cmp/cmpl/cmpi/cmpli (all with CR field + XER[SO]) |
| Load/Store integer | 14 | ✅ lwz/lwzu/lbz/lbzu/lhz/lhzu/lha/lhau/stw/stwu/stb/stbu/sth/sthu |
| Load/Store indexed | 8 | ✅ lwzx/lbzx/lhzx/lhax/stwx/stbx/sthx/lwbrx/sthbrx |
| Load/Store atomic | 2 | ✅ lwarx/stwcx. |
| Load/Store string | 2 | ✅ lswi/stswi |
| Load/Store FP | 8 | ✅ lfs/lfd/stfs/stfd + indexed variants |
| Branch unconditional | 2 | ✅ b/bl |
| Branch conditional | 5 | ✅ bc (all BO variants)/bdnz/bdz/bclr/bcctr |
| CR logical | 9 | ✅ mcrf/crand/cror/crxor/crnor/crandc/creqv/crorc/crnand |
| SPR/CR move | 5 | ✅ mfspr/mtspr (with XER pack/unpack)/mfcr/mtcrf/mftb |
| FP double arithmetic | 6 | ✅ fadd/fsub/fmul/fdiv/fmadd/fmsub/fnmadd/fnmsub |
| FP single arithmetic | 9 | ✅ fadds/fsubs/fmuls/fdivs/fmadds/fmsubs/fnmadds/fnmsubs/fres |
| FP move/convert | 7 | ✅ fmr/fneg/fabs/fnabs/frsp/fctiw/fctiwz/fsel/frsqrte |
| FP compare | 2 | ✅ fcmpu/fcmpo |
| FPSCR | 5 | ✅ mffs/mtfsf/mtfsfi/mtfsb0/mtfsb1/mcrfs — syncs ARM64 FPCR rounding |
| AltiVec (NEON) | 140 | ✅ Full VMX via AArch64 NEON intrinsics |
| Cache/Sync/NOP | 8 | ✅ dcbf/dcbst/dcbt/dcbtst/dcba/icbi/isync/sync/eieio |
| System | 4 | ✅ sc/mfmsr/eciwx/ecowx (terminators/NOPs) |
| **Total** | **285** | **+ all record forms (. suffix)** |

### FPU Coverage

| Feature | Status |
|---------|--------|
| Double-precision arithmetic | ✅ fadd/fsub/fmul/fdiv/fmadd/fmsub/fnmadd/fnmsub |
| Single-precision (round-to-single) | ✅ fadds/fsubs/fmuls/fdivs/fmadds/fmsubs/fnmadds/fnmsubs/fres |
| FP move/convert | ✅ fmr/fneg/fabs/fnabs/frsp/fctiw/fctiwz/fsel/frsqrte |
| FP compare → CR | ✅ fcmpu/fcmpo with XER[SO] |
| FPSCR rounding modes | ✅ PPC RN → ARM64 FPCR RMode mapping (nearest/zero/+inf/-inf) |
| FP load/store | ✅ lfs (single→double)/lfd/stfs (double→single)/stfd + indexed |
| FP exceptions | ⚠️ Not tracked (ARM64 defaults match PPC defaults) |

### XER (Carry/Overflow) Implementation

XER is a struct `{uint8 so, ov, ca, byte_count}` — NOT a packed uint32.
All JIT access uses byte-level LDRB/STRB at individual field offsets:
- `emit_read_xer_ca()`: LDRB from offset 902
- `emit_write_xer_ca_from_carry()`: CSET CS + STRB
- `emit_read_xer_so()`: LDRB from offset 900
- `mfspr XER`: packs 4 bytes → PPC 32-bit format
- `mtspr XER`: unpacks PPC format → 4 individual bytes

---

## BasiliskII 68K JIT

**Build:** ✅
**Interpreter:** ✅ Boots Mac OS 7.x, idle loop reached
**JIT optlev=0:** ✅ Full boot, 9.5B instructions, zero SEGVs
**JIT optlev=2:** ⚠️ SEGVs during early init from block handoff bug
**JIT harness:** 26/28 vectors pass (2 SR-only flag mismatches)

See `BasiliskII/src/uae_cpu_2021/compiler/` for the 68K → AArch64 JIT.

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
