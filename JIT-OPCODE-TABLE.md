# M68K JIT Opcode Coverage — ARM64

Generated: 2026-04-21T10:52:47Z


| Mnemonic | Variants | Inline | Mixed | C Helper | Interp | Status |
|----------|----------|--------|-------|----------|--------|--------|
| ABCD         |        2 |      2 |     0 |        0 |      0 | ✅ inline |
| ADD          |      104 |    104 |     0 |        0 |      0 | ✅ inline |
| ADDA         |       26 |     26 |     0 |        0 |      0 | ✅ inline |
| ADDX         |        6 |      6 |     0 |        0 |      0 | ✅ inline |
| AND          |       78 |     78 |     0 |        0 |      0 | ✅ inline |
| ANDSR        |        2 |      2 |     0 |        0 |      0 | ✅ inline |
| ASL          |        6 |      6 |     0 |        0 |      0 | ✅ inline |
| ASLW         |        7 |      7 |     0 |        0 |      0 | ✅ inline |
| ASR          |        6 |      6 |     0 |        0 |      0 | ✅ inline |
| ASRW         |        7 |      7 |     0 |        0 |      0 | ✅ inline |
| BCHG         |       20 |     20 |     0 |        0 |      0 | ✅ inline |
| BCLR         |       20 |     20 |     0 |        0 |      0 | ✅ inline |
| BFINS        |        6 |      0 |     6 |        0 |      0 | ✅ inline |
| BSET         |       20 |     20 |     0 |        0 |      0 | ✅ inline |
| BSR          |        3 |      3 |     0 |        0 |      0 | ✅ inline |
| BTST         |       22 |     22 |     0 |        0 |      0 | ✅ inline |
| Bcc          |       39 |     39 |     0 |        0 |      0 | ✅ inline |
| CHK          |       22 |     22 |     0 |        0 |      0 | ✅ inline |
| CLR          |       24 |     24 |     0 |        0 |      0 | ✅ inline |
| CMP          |       65 |     65 |     0 |        0 |      0 | ✅ inline |
| CMPA         |       24 |     24 |     0 |        0 |      0 | ✅ inline |
| CMPM         |        3 |      3 |     0 |        0 |      0 | ✅ inline |
| DBcc         |       14 |     14 |     0 |        0 |      0 | ✅ inline |
| DIVL         |       11 |     11 |     0 |        0 |      0 | ✅ inline |
| DIVS         |       11 |     11 |     0 |        0 |      0 | ✅ inline |
| DIVU         |       11 |     11 |     0 |        0 |      0 | ✅ inline |
| EOR          |       48 |     48 |     0 |        0 |      0 | ✅ inline |
| EORSR        |        2 |      2 |     0 |        0 |      0 | ✅ inline |
| EXG          |        3 |      3 |     0 |        0 |      0 | ✅ inline |
| EXT          |        3 |      3 |     0 |        0 |      0 | ✅ inline |
| FBcc         |        2 |      0 |     0 |        0 |      2 | 🐢 interpreter |
| FPP          |       12 |      0 |     0 |        0 |     12 | 🐢 interpreter |
| FScc         |        8 |      0 |     0 |        0 |      8 | 🐢 interpreter |
| JMP          |        7 |      7 |     0 |        0 |      0 | ✅ inline |
| JSR          |        7 |      7 |     0 |        0 |      0 | ✅ inline |
| LEA          |        7 |      7 |     0 |        0 |      0 | ✅ inline |
| LINK         |        2 |      2 |     0 |        0 |      0 | ✅ inline |
| LSL          |        6 |      6 |     0 |        0 |      0 | ✅ inline |
| LSLW         |        7 |      7 |     0 |        0 |      0 | ✅ inline |
| LSR          |        6 |      6 |     0 |        0 |      0 | ✅ inline |
| LSRW         |        7 |      7 |     0 |        0 |      0 | ✅ inline |
| MOVE         |      281 |    281 |     0 |        0 |      0 | ✅ inline |
| MOVE16       |        5 |      5 |     0 |        0 |      0 | ✅ inline |
| MOVE2C       |        1 |      0 |     1 |        0 |      0 | ✅ inline |
| MOVEA        |       24 |     24 |     0 |        0 |      0 | ✅ inline |
| MOVEC2       |        1 |      0 |     1 |        0 |      0 | ✅ inline |
| MULL         |       11 |     11 |     0 |        0 |      0 | ✅ inline |
| MULS         |       11 |     11 |     0 |        0 |      0 | ✅ inline |
| MULU         |       11 |     11 |     0 |        0 |      0 | ✅ inline |
| MV2SR        |       22 |     22 |     0 |        0 |      0 | ✅ inline |
| MVMEL        |       16 |     16 |     0 |        0 |      0 | ✅ inline |
| MVMLE        |       12 |     12 |     0 |        0 |      0 | ✅ inline |
| MVR2USP      |        1 |      1 |     0 |        0 |      0 | ✅ inline |
| MVUSP2R      |        1 |      1 |     0 |        0 |      0 | ✅ inline |
| NBCD         |        8 |      8 |     0 |        0 |      0 | ✅ inline |
| NEG          |       24 |     24 |     0 |        0 |      0 | ✅ inline |
| NEGX         |       24 |     24 |     0 |        0 |      0 | ✅ inline |
| NOP          |        1 |      0 |     0 |        0 |      1 | ⬜ no-op |
| NOT          |       24 |     24 |     0 |        0 |      0 | ✅ inline |
| OR           |       78 |     78 |     0 |        0 |      0 | ✅ inline |
| ORSR         |        2 |      2 |     0 |        0 |      0 | ✅ inline |
| PEA          |        7 |      7 |     0 |        0 |      0 | ✅ inline |
| RESET        |        1 |      0 |     0 |        0 |      1 | ⬜ no-op |
| ROL          |        6 |      6 |     0 |        0 |      0 | ✅ inline |
| ROLW         |        7 |      7 |     0 |        0 |      0 | ✅ inline |
| ROR          |        6 |      6 |     0 |        0 |      0 | ✅ inline |
| RORW         |        7 |      7 |     0 |        0 |      0 | ✅ inline |
| ROXL         |        6 |      6 |     0 |        0 |      0 | ✅ inline |
| ROXLW        |        7 |      7 |     0 |        0 |      0 | ✅ inline |
| ROXR         |        6 |      6 |     0 |        0 |      0 | ✅ inline |
| ROXRW        |        7 |      7 |     0 |        0 |      0 | ✅ inline |
| RTD          |        1 |      1 |     0 |        0 |      0 | ✅ inline |
| RTE          |        1 |      0 |     1 |        0 |      0 | ✅ inline |
| RTR          |        1 |      1 |     0 |        0 |      0 | ✅ inline |
| RTS          |        1 |      1 |     0 |        0 |      0 | ✅ inline |
| SBCD         |        2 |      2 |     0 |        0 |      0 | ✅ inline |
| STOP         |        1 |      0 |     1 |        0 |      0 | ✅ inline |
| SUB          |      104 |    104 |     0 |        0 |      0 | ✅ inline |
| SUBA         |       26 |     26 |     0 |        0 |      0 | ✅ inline |
| SUBX         |        6 |      6 |     0 |        0 |      0 | ✅ inline |
| SWAP         |        1 |      1 |     0 |        0 |      0 | ✅ inline |
| Scc          |      112 |    112 |     0 |        0 |      0 | ✅ inline |
| TAS          |        8 |      8 |     0 |        0 |      0 | ✅ inline |
| TRAPV        |        1 |      1 |     0 |        0 |      0 | ✅ inline |
| TST          |       35 |     35 |     0 |        0 |      0 | ✅ inline |
| UNLK         |        1 |      1 |     0 |        0 |      0 | ✅ inline |
| **TOTAL** | **1605** | **1581** | | **0** | **24** | |

## Summary

- **Inline ARM64**: 1581/1605 variants (98.5%) — 81 mnemonics
- **Mixed inline + C helper**: 0 mnemonics — 
- **C helper only**: 0 variants — none
- **Interpreter fallback**: 24 variants — FBcc, FPP, FScc
- **No-op (correct)**: NOP, RESET

### Status key

- ✅ **inline**: All variants emit ARM64 instructions directly into compiled blocks
- ⚡ **mixed**: Some variants inline, others use flush+call_helper for complex cases
- 🔧 **C helper**: All variants use flush+call_helper (native speed, function call overhead)
- 🐢 **interpreter**: Falls back to interpretive execution (block split)
- ⬜ **no-op**: Correct empty function body (NOP, RESET)
