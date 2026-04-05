# AArch64 JIT Bringup — BasiliskII on ARM64

## Overview

This document describes the work done to bring up the experimental AArch64 (ARM64) JIT compiler in BasiliskII, enabling native ARM64 code generation for m68k emulation. The JIT translates 68040 instructions into ARM64 native code at runtime, with two optimization levels:

- **L1 (optlev=1)**: All instructions fall through to interpreter with per-instruction spcflags checks. Safe but slow — no native codegen.
- **L2 (optlev=2)**: Instructions compile to native ARM64 code. Register allocator tracks values across instructions within a block. ~2-5× faster than L1.

The codebase is a fork of the Koenig/cebix/aranym JIT originally written for x86, ported to ARM (32-bit) by contributors, with an ARM64 backend added experimentally. Our work fixed **13 bugs** that prevented or destabilized L2 boot, bringing 9 of 15 opcode families to fully working native codegen. The remaining all-native failure has been narrowed substantially: it is **not** explained by the original byte-order bugs, not by verified per-instruction semantics, and not solely by masked interrupt/tick timing. The unresolved piece now appears to live in fully-native block execution/dispatch/cache behavior.

### Hardware

- **Board**: Orange Pi 6 Plus (CIX P1 SoC, 12 cores)
- **Arch**: AArch64, little-endian
- **OS**: Debian Trixie
- **Runtime**: Bun + Xvfb for headless testing

## Architecture

### JIT Pipeline

```
m68k instruction stream
        │
        ▼
┌─────────────────────┐
│  execute_normal()   │  Interpreter traces a block of m68k instructions,
│  (block tracing)    │  recording pc_hist[] entries and running each
│                     │  through the interpreter.
└─────────┬───────────┘
          │
          ▼
┌─────────────────────┐
│  compile_block()    │  For each instruction in pc_hist[]:
│  (code generation)  │  - If compiled handler exists → emit native ARM64 code
│                     │  - If no handler → emit interpreter fallback call
│                     │  - If barrier instruction → interpreter + endblock
└─────────┬───────────┘
          │
          ▼
┌─────────────────────┐
│  Native code block  │  Runs directly on ARM64 hardware.
│  (JIT cache)        │  Register allocator maps m68k regs → ARM64 regs.
│                     │  Block exit dispatches to next block via handler chain.
└─────────────────────┘
```

### Key Source Files

| File | Purpose |
|------|---------|
| `compiler/compemu_support_arm.cpp` | Main JIT compiler: block compilation, register allocator, barriers |
| `compiler/compemu_legacy_arm64_compat.cpp` | ARM64 compatibility layer: maps x86-style JIT primitives (sbb, adc, bt) to ARM64 |
| `compiler/compemu_midfunc_arm64.cpp` | ARM64 mid-level functions: register allocator API wrappers |
| `compiler/compemu_midfunc_arm64_2.cpp` | ARM64 native codegen: ADD, SUB, MOVE, shifts, memory access |
| `compiler/codegen_arm64.cpp` | ARM64 code emitter: raw instruction encoding (LDR, STR, ADD, etc.) |
| `compiler/compemu_arm.h` | ARM64-specific constants, register definitions, op_properties |
| `src/Unix/compemu.cpp` | Generated compiled handlers (2827 functions from gencomp_arm.c) |
| `newcpu.cpp` | Interpreter core, interrupt handling, managed IRQ delivery |
| `basilisk_glue.cpp` | TriggerInterrupt, intlev, managed IRQ model |

### Register Layout

| ARM64 Register | JIT Usage |
|---------------|-----------|
| x0 | REG_PAR1 / REG_RESULT (function call parameter/return) |
| x1 | REG_PAR2 / REG_PC_TMP |
| x2-x5 | REG_WORK1-4 (scratch, in `always_used`) |
| x6-x17 | Available for register allocator (guest registers D0-D7, A0-A7) |
| x18 | Platform reserved (in `always_used`) |
| x19-x26 | Callee-saved, available but unused |
| x27 | R_MEMSTART (base address of guest RAM) |
| x28 | R_REGSTRUCT (base address of `regs` struct) |
| ARM64 NZCV | Maps directly to m68k NZCV flags (same bit positions) |

### Memory Access

Guest memory is mapped as a contiguous region. Memory reads use `LDR [R_MEMSTART + guest_addr]` with byte-reversal (REV/REV16) for big-endian conversion:

```
Byte:  LDRB Wd, [x27, Wadr]           (no endian swap needed)
Word:  LDRH W1, [x27, Wadr] + REV16   (16-bit byte swap)
Long:  LDR  W1, [x27, Wadr] + REV     (32-bit byte swap)
```

## Bugs Found and Fixed

### Root Cause: Byte-Order Mismatch

The ARM64 JIT inherited code from the x86 backend where `HAVE_GET_WORD_UNSWAPPED` was defined, meaning opcodes were kept in host byte order (little-endian). On ARM64, this macro is undefined — `DO_GET_OPCODE()` returns the **logical** (big-endian) m68k opcode via `uae_bswap_16()`. Multiple subsystems incorrectly assumed raw host-order indices.

### Bug 1: IRQ Deliverability (2026-04-03)

**Symptom**: Managed-mode IRQ delivery latched pending interrupts while the interrupt mask (`regs.intmask=7`) was active, blocking all future delivery.

**Fix**: Only latch IRQs when they are actually deliverable (intlev > intmask). Verified via interrupt trace logging.

### Bug 2: Handler Register/Immediate Extraction (2026-04-03)

**Symptom**: Every L2 compiled handler was extracting wrong register numbers from opcodes.

**Root cause**: `HAVE_GET_WORD_UNSWAPPED` was still implicitly assumed in `compemu.cpp`. The extraction macros like `(opcode >> 8) & 7` expect byte-swapped opcodes, but ARM64's `DO_GET_OPCODE` returns logical opcodes needing `opcode & 7`.

**Fix**: `#undef HAVE_GET_WORD_UNSWAPPED` in `compemu.cpp` for ARM64, switching all 2827 compiled handlers to the non-swapped extraction paths.

### Bug 3: Interpreter Fallback Dispatch (2026-04-03)

**Symptom**: Every interpreter fallback called the wrong handler function.

**Root cause**: `cputbl[opcode]` instead of `cputbl[cft_map(opcode)]`. Since `cft_map()` is identity on ARM64 (no byte swap), this was harmless for some opcodes but wrong for opcodes where `table68k[opcode].handler ≠ opcode`.

**Fix**: Use `cputbl[cft_map(opcode)]` consistently.

### Bug 4: Flag Liveness Metadata (2026-04-03)

**Symptom**: Flag optimization made wrong decisions about which flags were live/dead, causing incorrect flag-setting behavior.

**Root cause**: `prop[cft_map(op)]` in the flag liveness backward analysis was consulting metadata for the wrong opcode.

**Fix**: Corrected all `prop[]` indexing in the flag optimizer to use consistent `cft_map()` mapping.

### Bug 5: L2 Compiled Handler Dispatch (2026-04-03)

**Symptom**: Compiled handlers were dispatched for the wrong opcode variants.

**Root cause**: `comptbl[opcode]` instead of `comptbl[cft_map(opcode)]`.

**Fix**: Use `comptbl[cft_map(opcode)]` for compiled handler dispatch.

### Bug 6: `is_const_jump` Block-End Detection (2026-04-03)

**Symptom**: Blocks were splitting at wrong points during compilation.

**Root cause**: `prop[uae_bswap_16(opcode)].cflow` used a different byte-order convention than the rest of the code.

**Fix**: Corrected the indexing to match the `cft_map` convention.

### Bug 7: ARM64 Native Shift Codegen (2026-04-04)

**Symptom**: ASL, ROR with byte/word operands produced wrong results.

**Root cause**: `jff_ASL_w_imm`, `jff_ROR_b_imm`, `jff_ROR_w_imm` had incorrect ARM64 instruction encoding for sub-32-bit shifts. Previously hidden because wrong dispatch (bugs 2-5) routed shift instructions to wrong handlers.

**Fix**: Corrected the shift/rotate immediate values for byte and word operand sizes.

### Bug 8: X Flag Format Mismatch (2026-04-04)

**Symptom**: After a compiled instruction set X=1 (carry), the interpreter always read X=0.

**Root cause**: `DUPLICACTE_CARRY` macro used `CSET` which stores 0 or 1 (bit 0). But the interpreter reads X via `GET_XFLG() = (regflags.x >> 29) & 1`, expecting the value at bit 29 (`FLAGVAL_X = 0x20000000`).

**Fix**: Added `LSL_wwi(x, x, 29)` after CSET in `DUPLICACTE_CARRY` to shift the flag to the interpreter's expected position.

### Bug 9: BTST Corrupts X Flag (2026-04-04)

**Symptom**: BTST/BCHG/BCLR/BSET instructions incorrectly modified the X flag.

**Root cause**: These instructions use `sbb_l(s,s)` internally to convert the carry (bit-test result) to a register value. The `sbb_l` wrapper called `legacy_copy_carry_to_flagx()`, which is an x86 convention (carry IS the X flag on x86) that doesn't apply to ARM64 where X is stored separately.

**Fix**: Removed `legacy_copy_carry_to_flagx()` from `sbb_b/w/l` wrappers.

### Bug 10: ADDX Reads Wrong X Flag (2026-04-04)

**Symptom**: ADDX/SUBX instructions read the current ARM64 carry instead of the saved X flag.

**Root cause**: `adc_l` wrapper called `legacy_copy_carry_to_flagx()` which overwrote `regflags.x` with the current ARM64 carry before the ADDX handler read it. On x86, carry IS X, so this was a no-op. On ARM64, it clobbered the saved X value.

**Fix**: Removed `legacy_copy_carry_to_flagx()` from `adc_b/w/l` wrappers. The ADDX handler correctly reads X from `regflags.x` via `readreg(FLAGX)`.

### Bug 11: COPY_CARRY Stores Garbage in X Flag (2026-04-05)

**Symptom**: Block-level trace comparison showed identical register inputs at a byte-copy loop entry, but different D1/D2 outputs. The X flag contained values like `0x80000000` (N bit) or `0x90000000` (N+V bits) instead of valid `0` or `0x20000000` (carry bit).

**Root cause**: `COPY_CARRY()` was defined as `regflags.x = regflags.nzcv >> (FLAGBIT_C - FLAGBIT_X)`. Since `FLAGBIT_C == FLAGBIT_X == 29`, the shift was 0, copying the **entire** NZCV value (including N, Z, V garbage) into `regflags.x`. While the interpreter's `GET_XFLG()` correctly extracted only bit 29, the JIT's `readreg(FLAGX)` read the raw value and used it as an arithmetic addend in ADDX/SUBX.

**Fix**: Changed `COPY_CARRY()` to `regflags.x = regflags.nzcv & FLAGVAL_C`, masking to only the carry bit (bit 29). Also added UBFX normalization at compiled section entry (`init_comp`) to convert from bit-29 format to the JIT's 0/1 format, and LSL conversion in `tomem()`/`writeback_const()` to convert back.

### Bug 12: Masked IRQs Still Split Compiled Blocks (2026-04-05)

**Symptom**: Even with `sr=2708` / `regs.intmask=7` (level-1 interrupts masked), the async 60Hz path could still knock the JIT out of native execution at block-shape-dependent points.

**Root cause**: `TriggerInterrupt()` unconditionally set `SPCFLAG_INT`. Compiled code treats any `spcflags` bit as a block-break condition, so masked interrupts were still creating non-architectural exits from native blocks. The CPU only discovered the IRQ was masked later in `m68k_do_specialties()`/`intlev()`.

**Fix**: In managed/deferred JIT mode, `TriggerInterrupt()` now raises `SPCFLAG_INT` only when level-1 is actually deliverable (`regs.intmask < 1`). Pending masked ticks remain in `InterruptFlags`. `MakeFromSR()` now re-raises `SPCFLAG_INT` when an SR/intmask change makes a pending level-1 interrupt deliverable.

### Bug 13: Partial JIT Exits Charged Full-Block Cycles (2026-04-05)

**Symptom**: Mid-block exits (inter-instruction spcflags check, interpreter barrier exit, exception exit, runtime helper barrier) charged the entire block's `scaled_cycles(totcycles)` even when only a prefix of the block had retired.

**Root cause**: The cold exit paths used the block total instead of the retired prefix, making countdown/timing depend on block shape even when the exit point was earlier in the block.

**Fix**: All mid-block exits now charge `scaled_cycles((i + 1) * 4 * CYCLE_UNIT)`, i.e. only the retired instruction prefix.

## Block-Level Trace Analysis

The comprehensive block-level trace comparison was the key diagnostic that identified bugs 11 and the remaining memory divergence. The technique:

1. **Full register trace**: Log all 16 registers + SR + NZCV + X at every `execute_normal()` entry via `B2_JIT_PCTRACE=N`
2. **Dual-trace comparison**: Run identical configs except for the family under test
3. **Sequence alignment**: Match traces by PC value, skipping block-boundary differences
4. **First-divergence detection**: Find the earliest point where registers differ at matching PCs

### Key Findings: Semantics Verified, Structural Failure Remains

The trace and verification work now shows two things simultaneously:

- All 16 registers + NZCV + X match for hundreds of aligned trace points across good-vs-all-native comparisons
- L1-vs-L2 register comparison found **zero divergences** across 2000 trace samples
- Family-d runtime tracing and memory-write verification found **zero mismatches** in compiled register/flag results and guest memory writes for the instructions instrumented
- A real low-memory content divergence still shows up later in the boot path, but it is no longer well-explained by a simple single-instruction semantic bug

In other words: the earlier byte-order / flag / handler bugs were real and are fixed, but the remaining failure mode behaves like a **structural all-native execution problem** rather than a straightforward per-opcode arithmetic or addressing bug.

### Current Working Hypothesis

The remaining all-native failure likely involves one or more of:

- direct block chaining / next-handler selection
- cache-tag or checksum/invalidation behavior that only appears in the fully-native graph
- a block-layout-dependent control-flow issue that does not show up in isolated per-instruction verification

The earlier `ADDA.L (d16,A3),A3` suspicion remains a useful breadcrumb, but is no longer treated as a proven root cause.

## Structural Changes

### Per-Instruction Interpreter Barriers

Instead of demoting entire blocks to L1 when they contain control-flow instructions, we implemented per-instruction interpreter barriers via `jit_force_interpreter_barrier_opcode()`. Specific instruction types force an interpreter fallback within L2 blocks:

- **Control flow**: Bcc, BRA, BSR, JMP, JSR, RTS, RTE, RTR, RTD, TRAPV
- **Multi-register**: MOVEM
- **Traps**: A-line (0xAxxx), EmulOps (0x71xx)
- **SR modifiers**: MV2SR.W, ANDI/ORI/EORI to SR, selected CCR operations

Instructions before the barrier compile natively; the barrier instruction itself runs through the interpreter and ends the block.

### Managed IRQ Delivery (`B2_JIT_MANAGED_IRQ=1`)

Replaced the SIGUSR1-based async interrupt injection with a deferred safe-point model (similar to the original x86 JIT). Interrupts are consumed at block boundaries via `spcflags` checks rather than asynchronous signal delivery.

A later fix tightened this further: in managed mode, **masked** level-1 interrupts no longer set `SPCFLAG_INT` immediately. They remain pending in `InterruptFlags` until SR/intmask changes make them deliverable. This removes a real block-boundary side-channel, although it did not fully solve the all-native stall.

### Lightweight Inter-Instruction Spcflags Check

Between each compiled instruction, the JIT emits a conditional spcflags check:

```
; Hot path (spcflags == 0): 2 instructions
LDR  W1, [x28, #spcflags_offset]
CBZ  W1, .skip

; Cold path (interrupt pending): conditional flush
STR  Wd0, [x28, #d0_offset]    ; save dirty D0
STR  Wd1, [x28, #d1_offset]    ; save dirty D1
...                              ; (only emitted for actually-dirty registers)
MRS  X2, NZCV                   ; save flags
STR  W2, [regflags.nzcv]
MOV  X1, #next_pc               ; sync PC
STR  X1, [regs.pc_p]
SUB  countdown, countdown, #cycles
B    popall_do_nothing           ; exit block

.skip:
; continue to next instruction
```

The cold path saves registers WITHOUT modifying compile-time allocator state, so the hot path continues with the full register allocator optimization.

### `configure.ac` Build Flag

Added `--enable-aarch64-jit-experimental` to the autoconf build system, enabling the ARM64 JIT backend with appropriate compiler flags (no LTO, which strips the structural gate checks).

## Testing Infrastructure

### Environment Variables

| Variable | Purpose |
|----------|---------|
| `B2_JIT_MANAGED_IRQ=1` | Use deferred IRQ model instead of SIGUSR1 |
| `B2_JIT_BARRIER_FAMILIES="0,1,2,..."` | Force specific opcode families through interpreter barriers |
| `B2_JIT_MAXRUN=N` | Limit block length to N instructions |
| `B2_JIT_NO_FLAG_OPT=1` | Disable flag liveness optimization |
| `B2_JIT_FLUSH_BETWEEN=1` | Reset register allocator between compiled instructions |
| `B2_JIT_PCTRACE=N` | Log first N block entry PCs with register state |
| `B2_JIT_VERIFY_LIMIT=N` | Per-instruction runtime verification (compare compiled vs interpreter) |
| `B2_JIT_SYNC_TICKS=1` | Experimental synchronous tick model driven from JIT dispatch returns |
| `B2_JIT_L2_ONLY=1` | Force all blocks to optlev=2 |
| `B2_JIT_MAX_OPTLEV=N` | Cap maximum optimization level |

### Runtime Verification Framework

The per-instruction verification (`B2_JIT_VERIFY_LIMIT`) works by:

1. Before compiled instruction: flush registers, emit call to `jit_verify_pre()` which saves all guest state
2. After compiled instruction: flush registers, emit call to `jit_verify_post()` which restores pre-state, runs the interpreter for the same instruction, and compares register values + flags

This confirmed 0 mismatches for all tested instructions across all opcode families.

### Per-Family Bisection

The `B2_JIT_BARRIER_FAMILIES` variable allows testing each opcode family independently:

| Family | Instructions | Boot Status |
|--------|-------------|-------------|
| 0 | ORI/ANDI/SUBI/ADDI/CMPI/BTST/BCHG/BCLR/BSET | ⏳ All-native structural stall |
| 1 | MOVE.B | ⏳ All-native structural stall |
| 2 | MOVE.L | ⏳ All-native structural stall |
| 3 | MOVE.W | ⏳ All-native structural stall |
| 4 | CLR/NEG/NOT/TST/PEA/LEA/EXT/SWAP/LINK/UNLK | ✅ Boots |
| 5 | ADDQ/SUBQ/Scc/DBcc | ⏳ All-native structural stall |
| 6 | Bcc/BRA/BSR | ✅ Boots (barriers) |
| 7 | MOVEQ | ✅ Boots |
| 8 | OR/DIV/SBCD | ✅ Boots |
| 9 | SUB/SUBA/SUBX | ✅ Boots |
| a | A-line traps | ✅ Boots (barriers) |
| b | CMP/CMPA/EOR | ✅ Boots |
| c | AND/MUL/ABCD/EXG | ✅ Boots |
| d | ADD/ADDA/ADDX | ⏳ All-native structural stall |
| e | Shifts/Rotates | ✅ Boots |

### Boot Progress Metrics

- **DiskStatus**: Count of SCSI DiskStatus calls (49 = full boot)
- **SCSIGet**: Count of SCSI Get operations (14 = full boot)
- **Apple logo**: Colorful pixels in top-left 80×20 crop of framebuffer
- **optlev2**: Count of blocks compiled at optimization level 2

### Headless Testing

All testing uses Xvfb + VNC for headless Mac OS boot verification:

```bash
Xvfb :99 -screen 0 640x480x24 &
SDL_VIDEODRIVER=x11 DISPLAY=:99 B2_JIT_MANAGED_IRQ=1 \
  ./BasiliskII --config prefs
# Screenshot via xwd + pnmtopng
# Desktop detection via PIL color analysis
```

## Current Status

### What Works

With the 9 working opcode families (4,6,7,8,9,a,b,c,e) compiling natively:
- Mac OS 7.5.5 boots to Finder desktop reliably
- Speedometer benchmark runs (Graphics score: 210.368 at L1)
- VNC remote display functional
- Managed IRQ delivery stable

### Remaining Issue: All-Native Stall After Early Native Graph Build

The 6 remaining families (0,1,2,3,5,d) still prevent a fully-native boot, but the diagnosis is now narrower than “memory divergence from a bad instruction.” Latest results:

- Good gated config (`B2_JIT_BARRIER_FAMILIES="0,1,2,3,5,d,f"`) still reaches healthy boot progress: `DiskStatus=42`, `SCSIGet=14`
- All-native managed-IRQ config still stalls very early with no disk progress: `DiskStatus=0`, `SCSIGet=0`, after only ~174 compiled L2 blocks
- Experimental synchronous ticks (`B2_JIT_SYNC_TICKS=1`) do **not** resolve the stall; the all-native run still stops around ~181 compiled L2 blocks
- Fixing masked IRQ exits (bug 12) and exact partial-exit accounting (bug 13) was necessary cleanup, but did **not** restore all-native boot

This strongly suggests the remaining problem is inside the fully-native block graph itself: dispatch, chaining, cache tags, checksum/invalidation, or some other structural behavior that only appears when the last 6 families are allowed to participate natively.

### Diagnosis Path Forward

1. **Instrument direct block chaining**: Log `get_handler()` / `cache_tags` targets around the first all-native stall (~L2 block 174)
2. **Compare block-manager state**: Diff `blockinfo`, checksum state, and cache-tag ownership between good and all-native runs
3. **Capture first divergent native transition**: Record block entry PC, chosen successor handler, and resolved next PC for the earliest all-native-only control-flow split
4. **Only then revisit specific opcode families**: Treat individual family handlers as suspects only if the block-management layer checks out

## Build Instructions

```bash
cd BasiliskII/src/Unix
NO_CONFIGURE=1 ./autogen.sh
./configure --enable-sdl-video=yes --enable-sdl-audio=yes \
            --enable-jit-compiler --enable-aarch64-jit-experimental \
            --with-sdl2 --disable-vosf
make -j$(nproc)
```

### Running

```bash
B2_JIT_MANAGED_IRQ=1 ./BasiliskII --config /path/to/prefs
```

### Stable Binaries

| Binary | Description |
|--------|-------------|
| `BasiliskII-jit-stable` | L1 all-barrier (reliable, slow) |
| `BasiliskII-jit-deferredirq` | L2 with managed IRQ (current development) |
| `BasiliskII-nojit` | Pure interpreter (reference) |

## Appendix: Commit History

```
defad98a aarch64 jit: gate masked IRQ exits and charge partial exits exactly
63341c97 aarch64 jit: fix COPY_CARRY to mask carry bit only (bug 11)
b81dae11 aarch64 jit: fix FLAGX format conversion at interpreter/JIT boundary
37ee2faa aarch64 jit: simplify inter-instruction check (remove incorrect tick code)
d4a4e8a4 aarch64 jit: add deterministic tick counting between compiled instructions
3936ec36 docs: add comprehensive AArch64 JIT bringup document
62cbcbea aarch64 jit: lightweight spcflags check between compiled instructions
a1fe6e01 aarch64 jit: fix X flag bugs (9-10) + DUPLICACTE_CARRY bit-29 format
6899bef7 aarch64 jit: add inter-instruction spcflags check, barrier families env
7edf511a docs: add AArch64 JIT status, build instructions, and bug list
a3d13d89 aarch64: disable LTO in build, default to optlev=2
4d70bdaf aarch64 jit: per-instruction barriers for true L2, VNC coordinate fix
75c16502 aarch64 jit: fix LTO stripping structural gate checks
09602ee9 aarch64 jit: split L2_ONLY gate into structural and semantic layers
0fe9940b aarch64 jit: fix prop[] and comptbl[] byte-order indexing
e253da6f aarch64 jit: fix interpreter fallback dispatch byte-order bug
5b79e10a aarch64 jit: fix IRQ deliverability bug and byte-order extraction
e6d9f37a macemu: fix legacy bitop carry semantics on arm64
5fa96e49 macemu: preserve nzcv in legacy set_zero on arm64
```
