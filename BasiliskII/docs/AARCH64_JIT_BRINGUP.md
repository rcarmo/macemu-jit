# AArch64 JIT Bringup — BasiliskII on ARM64

## Overview

This document describes the work done to bring up the experimental AArch64 (ARM64) JIT compiler in BasiliskII, enabling native ARM64 code generation for m68k emulation. The JIT translates 68040 instructions into ARM64 native code at runtime, with two optimization levels:

- **L1 (optlev=1)**: All instructions fall through to interpreter with per-instruction spcflags checks. Safe but slow — no native codegen.
- **L2 (optlev=2)**: Instructions compile to native ARM64 code. Register allocator tracks values across instructions within a block. ~2-5× faster than L1.

The codebase is a fork of the Koenig/cebix/aranym JIT originally written for x86, ported to ARM (32-bit) by contributors, with an ARM64 backend added experimentally. Since the initial bringup, the work has fixed a long series of structural and semantic bugs: byte-order mismatches, IRQ deliverability, legacy carry/X handling, boundary cycle charging, verifier misuse, and ARM64 endblock slow-path bugs. The remaining all-native failure has now been narrowed much further: the immediate pure-L2 bad-PC crash is no longer best explained by the old helper-chain overshoot theory, but by a **native short-BRA (`BRAQ.B`) path that is still unsafe on ARM64**.

## Current frontier (2026-04-08)

The most important updates from the latest round are:

### 1. The old block-verifier overshoot result was wrong

The original block-level verifier was accidentally running native code from the interpreter's **post-block state** instead of the true entry state. That produced fake `+2/+4` successor-style overshoots such as:

- `0x040b3566 -> ...35c8`
- `0x040b35c8 -> ...3580`
- `0x040b34a8 -> ...3496`

This was fixed by:

- capturing a real pre-block entry snapshot before the interpreter trace loop runs
- replaying the interpreter block from entry state under the verifier
- running the compiled block from the same entry state

After that fix, the old “compiled block ran one or two instructions too far” conclusion was invalidated.

### 2. ARM64 endblock slow-path bugs were real and are now fixed

Two concrete ARM64 contract bugs were identified and fixed in dispatcher/chain handoff paths:

1. **setpc trampolines only updated `regs.pc_p`**
   - `popall_execute_normal_setpc`
   - `popall_check_checksum_setpc`
   - `popall_exec_nostats_setpc`

   These now rebuild canonical PC state instead of entering C/interpreter code with stale `regs.pc` / `regs.pc_oldp`.

2. **ARM64 endblock helpers used brittle negative-branch skip counts**
   - `compemu_raw_endblock_pc_inreg()`
   - `compemu_raw_endblock_pc_isconst()`

   These were rewritten to use explicit patched hot/slow branch layout, removing the hard-coded skip-count contract.

These fixes removed a real class of `bad pc_p` / `exec_normal bad` failures from the safe runtime.

### 3. Corrected verifier result for the old hot helper chain

With the fixed verifier, the earlier helper chain is mostly clean in targeted runs:

- `0x040b3566` → matches in targeted verifier runs
- `0x040b35c6` → matches in targeted verifier runs
- `0x040b35c8` → matches in targeted verifier runs
- `0x040b34a8` → matches in targeted verifier runs

The one remaining context-sensitive verifier mismatch in that neighborhood is:

- `0x040b357c`

and it depends on whether that code is traced as a 1-op block or as the full `0241 4ed3` 2-op form.

So the old `0x040b3566 -> 0x040b35c6 -> 0x040b35c8` chain is no longer the best lead for the immediate pure-L2 crash.

### 4. Pure L2 everywhere still crashes

All hardcoded barriers were temporarily removed, including:

- DBcc
- JMP / JSR
- Bcc / BRA / BSR
- EmulOps
- MOVEQ
- MOVEM
- SR/CCR modifiers

The pure-L2 run really was pure L2:

- `optlev=[2]`

and it failed with the old bad-PC family:

- `regs.pc = 0xffffffff`
- `pc_p = 0x10fffffff`
- `exec_normal bad`
- `bad pc_p`

So at least one removed barrier was hiding a real native codegen bug.

### 5. Barrier binary search localizes the immediate pure-L2 crash to short BRA

A systematic barrier binary search was run by restoring subsets of the removed barriers via env-controlled tokens.

Results:

- restoring `branch` prevented the immediate pure-L2 bad-PC crash
- restoring only `jsr` did **not** prevent it
- splitting `branch` further showed:
  - restoring only `braq` prevented the crash
  - restoring only `braw` did not
  - restoring only `bral` did not
  - restoring only `bsr` did not
  - restoring only `bcc` did not

So the crashing native branch-family path is specifically:

> **short BRA (`BRAQ.B`)**

### 6. Direct handler-side BRA arithmetic was not the whole bug

A direct fix attempt was made in the generated BRA handlers (`op_6000/op_6001/op_60ff`, ff/nf) to simplify how branch targets are formed, but pure-L2 still reproduced the `regs.pc=ffffffff` failure.

So the remaining short-BRA problem is probably **not just the obvious displacement arithmetic in the emitted handler body**. The deeper problem is more likely in the constant-jump / block-follow / block-concatenation semantics that native short BRA relies on.

### 7. Current safe state

To keep the runtime safe while continuing investigation:

- short BRA (`BRAQ.B`) is barriered again by default
- other hardcoded barriers remain removed unless explicitly restored by env
- current short smokes show:
  - `optlev=[2]`
  - no `bad pc_p`
  - no `exec_normal bad`
  - no SIGSEGV/SIGBUS in the 20s safe runtime check

### 8. What is still not solved

Even in the current safe state:

- Xvfb visual checks are still black
- no `DiskStatus`
- no `SCSIGet`
- no `SDL_VIDEOINT`
- no `SDL_PRESENT`

So the current state is:

> the immediate pure-L2 crash family is localized to short BRA and fenced off again, but boot still does not reach visible Mac OS progress.

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

### Bug 14: Legacy `adc/sbb` Helpers Used FLAGX Instead of Carry (2026-04-05)

**Symptom**: The first hard all-native semantic divergence appeared in the ROM table-walk loop at `0x0400091e` / `0x04000922`, involving:

- `MOVEA.W (A2)+,A3`
- `BTST.L D3,D0`

The all-native run took the wrong path through the table while the gated run took the correct path.

**Root cause**: The ARM64 legacy compat layer incorrectly mapped x86-style helper names:

- `adc_* -> ADDX_*`
- `sbb_* -> SUBX_*`

That is wrong. The generated JIT uses legacy `adc/sbb` both for true carry/borrow chains and for bit-test idioms like `bt_l_rr(...); sbb_l(s,s);`. These helpers must consume the **native carry flag**, not the m68k X flag (`FLAGX`). Using ADDX/SUBX silently substituted X-flag semantics where x86 carry semantics were required.

**Fix**: Replaced the aliases with real ARM64 carry-based implementations in `compemu_legacy_arm64_compat.cpp` using `ADCS` / `SBCS`, including correct handling of the JIT's `flags_carry_inverted` convention.

### Bug 15: FLAGX Memory Normalization Corrupted Interpreter Boundaries (2026-04-05)

**Symptom**: After bug 14, targeted runtime verification of the transform helper block at `0x040b3566` showed that the compiled/native result matched the interpreter for registers and flags, but `regflags.x` could still be left in raw JIT `0/1` format in memory instead of the interpreter's bit-29 format.

**Root cause**: At compiled-section entry, the ARM64 bringup code normalized `regflags.x` in memory from interpreter format (bit 29) to JIT format (`0/1`). If a compiled block subsequently **read** X but never dirtied/wrote it back, the block could exit to interpreter/C code with `regflags.x` still in JIT format. Later interpreter/helper code that expected bit-29 X then consumed the wrong value.

**Fix**: Removed the eager in-memory normalization of `regflags.x` and instead taught register loads of `FLAGX` to decode bit 29 into JIT `0/1` on load (`do_load_reg()` special case). This keeps memory permanently in interpreter format and only uses JIT format in native registers.

## Block-Level Trace Analysis

The comprehensive block-level trace comparison was the key diagnostic that identified bugs 11 and the remaining memory divergence. The technique:

1. **Full register trace**: Log all 16 registers + SR + NZCV + X at every `execute_normal()` entry via `B2_JIT_PCTRACE=N`
2. **Dual-trace comparison**: Run identical configs except for the family under test
3. **Sequence alignment**: Match traces by PC value, skipping block-boundary differences
4. **First-divergence detection**: Find the earliest point where registers differ at matching PCs

### Key Findings: Semantics Verified, Then Deeper Semantic Bugs Found

The trace and verification work now shows a layered picture:

- All 16 registers + NZCV + X matched for hundreds of aligned trace points across earlier good-vs-all-native comparisons
- L1-vs-L2 register comparison found **zero divergences** across 2000 trace samples for the instruction subsets instrumented
- Family-d runtime tracing and memory-write verification found **zero mismatches** in compiled register/flag results and guest memory writes for the instructions instrumented
- Despite that, a real semantic divergence was later found in the early ROM table-walk loop at `0x0400091e` / `0x04000922`, and was fixed by bug 14 (`adc/sbb` carry semantics)
- After bug 14, the all-native run gets further, and the next first matched-PC divergence moves deeper into the transformed helper path feeding `0x040b34dc`

So the remaining issue is no longer the original “everything is structurally wrong” hypothesis. At least one more genuine semantic bug existed after the first 13 fixes, and the next failure now appears to be in a narrower transformed-helper region rather than in generic interrupt timing.

### Current Working Hypothesis

The current hottest area is the transformed helper path around `0x040b3566..0x040b3636`, which feeds the later divergence at `0x040b34dc`. That region includes byte-oriented transforms and `ADDX.B` operations (`0x040b3622`, `0x040b3624`) and appears to participate in producing the wrong final byte pattern before the `MOVE.L (A3)+,D0 / SUB.L (A3)+,D0` check at `0x040b34dc`.

Potential remaining causes now include:

- another carry/X/flag interaction inside the transformed helper loop
- a semantic bug in the transformed byte/rotate/addx path
- less likely, a deeper block-management issue that only manifests after the bug-14 fix lets execution reach this later region

The earlier `ADDA.L (d16,A3),A3` suspicion remains historical context, but is no longer the primary lead.

## Structural Changes

### Per-Instruction Interpreter Barriers

The barrier policy changed during the latest isolation work.

Historically, many control-flow / trap / state-modifying instructions were hard-barriered in `jit_force_interpreter_barrier_opcode()`. To test pure L2 properly, those hardcoded barriers were removed and replaced with env-controlled restoration tokens such as:

- `branch`, `bra`, `braq`, `braw`, `bral`, `bsr`, `bcc`
- `jsr`, `jmp`, `ret`
- `movem`, `aline`, `emulop`, `sr`, `moveq`, `dbcc`

The current default safety state is narrower:

- **short BRA (`BRAQ.B`) remains barriered by default** because removing it reproduces the immediate pure-L2 `regs.pc=ffffffff` / `bad pc_p` crash
- other formerly hardcoded barriers are now restored only when explicitly requested for bisection/debugging

When a barrier is active, instructions before it still compile natively; the barrier instruction itself runs through the interpreter and the block exits through the normal dispatcher path.

### Managed IRQ Delivery (`B2_JIT_MANAGED_IRQ=1`)

Replaced the SIGUSR1-based async interrupt injection with a deferred safe-point model (similar to the original x86 JIT). Interrupts are consumed at block boundaries via `spcflags` checks rather than asynchronous signal delivery.

A later fix tightened this further: in managed mode, **masked** level-1 interrupts no longer set `SPCFLAG_INT` immediately. They remain pending in `InterruptFlags` until SR/intmask changes make them deliverable. This removes a real block-boundary side-channel, although it did not fully solve the all-native stall.

### Concurrent External Actors and Async Inputs

The JIT does not run in isolation. In the current SDL + pthread build, several host-side actors continue running while the CPU thread executes native blocks:

| Actor | Source | What it does | How it can affect L2 |
|---|---|---|---|
| CPU/JIT thread | `Start680x0()` → `m68k_compile_execute()` | Executes native blocks, fallbacks, exceptions, IRQ services | Primary execution engine |
| 60Hz tick thread | `src/Unix/main_unix.cpp` `tick_func()` | Real-time `one_tick()` every ~16.625 ms | Sets `INTFLAG_60HZ` / `INTFLAG_1HZ`, calls `TriggerInterrupt()`, pumps SDL events from main-thread context |
| SDL redraw/input thread | `src/SDL/video_sdl2.cpp` `redraw_func()` | Runs `handle_events()` + `video_refresh()` at ~60 Hz | Host input can raise `INTFLAG_ADB`; video work changes host scheduling and wall-time cost |
| XPRAM watchdog thread | `src/Unix/main_unix.cpp` `xpram_func()` | Periodic XPRAM save | Minor host scheduling / I/O noise |
| VNC thread (optional) | `src/SDL/vnc_server.cpp` | Background VNC snapshot / event processing | Extra host CPU load and timing pressure |
| SDL audio callback (optional) | `src/SDL/audio_sdl.cpp` | Consumes audio ring buffer | Can interact with `INTFLAG_AUDIO` and host scheduling |
| Ethernet RX thread (optional) | `src/ether.cpp` | Receives UDP/network packets asynchronously | Can raise `INTFLAG_ETHER` |
| Host renderer / compositor threads | external to BasiliskII | GPU / window-system work | Add wall-time jitter without direct guest-state mutation |

All asynchronous sources converge on:

- `InterruptFlags`
- `TriggerInterrupt()`
- `regs.spcflags`
- later `M68K_EMUL_OP_IRQ` service in `src/emul_op.cpp`

That IRQ service then fans back out into guest-visible work:

- `TimerInterrupt()`
- `VideoInterrupt()`
- `DoVBLTask`
- `SonyInterrupt()` / `DiskInterrupt()` / `CDROMInterrupt()`
- `ADBInterrupt()`
- `EtherInterrupt()`
- `AudioInterrupt()`

This means L2 can fail even when local architectural state matches at a probe site, simply because block shape changes **when** the CPU thread returns to a safe point and therefore **when** async work becomes visible.

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
| `B2_JIT_VERIFY_LIMIT=N` | Historical/legacy per-instruction verification knob |
| `B2_JIT_VERIFY_PCS=pc[-pc],...` | Targeted native-vs-interpreter verification for selected compiled instruction PCs |
| `B2_JIT_SYNC_TICKS=1` | Experimental synchronous tick model driven from JIT dispatch returns |
| `B2_JIT_L2_ONLY=1` | Force all blocks to optlev=2 |
| `B2_JIT_MAX_OPTLEV=N` | Cap maximum optimization level |
| `B2_JIT_FLUSH_EACH_OP=1` | Diagnostic: canonicalize guest state after every compiled opcode |
| `B2_JIT_FLUSH_OP_PCS=pc[-pc],...` | Diagnostic: canonicalize guest state only after selected compiled opcode PCs |

### Runtime Verification Framework

The per-instruction verification (`B2_JIT_VERIFY_LIMIT`) works by:

1. Before compiled instruction: flush registers, emit call to `jit_verify_pre()` which saves all guest state
2. After compiled instruction: flush registers, emit call to `jit_verify_post()` which restores pre-state, runs the interpreter for the same instruction, and compares register values + flags

This confirmed 0 mismatches for all tested instructions across all opcode families.

### Per-Family Bisection

The older `B2_JIT_BARRIER_FAMILIES` variable is still useful for coarse family testing, but the latest work bisected the crashing control-flow path more precisely with env-controlled barrier restoration tokens.

| Family | Instructions | Current status |
|--------|-------------|-------------|
| 0 | ORI/ANDI/SUBI/ADDI/CMPI/BTST/BCHG/BCLR/BSET | native active in current L2 tests |
| 1 | MOVE.B | native active in current L2 tests |
| 2 | MOVE.L | native active in current L2 tests |
| 3 | MOVE.W | native active in current L2 tests |
| 4 | CLR/NEG/NOT/TST/PEA/LEA/EXT/SWAP/LINK/UNLK | native active |
| 5 | ADDQ/SUBQ/Scc/DBcc | native active in current tests unless `dbcc` barrier is explicitly restored |
| 6 | Bcc/BRA/BSR | immediate pure-L2 crash localized to **short BRA (`BRAQ.B`)**; short BRA is barriered again by default |
| 7 | MOVEQ | native active in current L2 tests |
| a | A-line traps | native active unless `aline` barrier is explicitly restored |
| b | CMP/CMPA/EOR | native active |
| c | AND/MUL/ABCD/EXG | native active |
| d | ADD/ADDA/ADDX | native active in current L2 tests |
| e | Shifts/Rotates | native active |

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

- Managed IRQ delivery remains stable in the current safe build.
- The fixed verifier now gives trustworthy whole-block comparisons instead of successor-state artifacts.
- Current safe short smokes run with:
  - `optlev=[2]`
  - no `bad pc_p`
  - no `exec_normal bad`
  - no SIGSEGV / SIGBUS

### Remaining Issue

Boot still does **not** reach visible Mac OS progress in the current safe configuration:

- Xvfb capture remains black
- `DiskStatus=0`
- `SCSIGet=0`
- `SDL_VIDEOINT=0`
- `SDL_PRESENT=0`

So there are now **two separated facts**:

1. the immediate pure-L2 bad-PC crash has been localized to native short BRA and fenced off again
2. even with that crash fenced off, the machine still does not boot to a visible desktop

### Most useful current interpretation

The old broad “async/timing explains everything” hypothesis is no longer the best summary.

The strongest current picture is:

- a real native short-BRA bug exists on ARM64 L2
- that bug is severe enough to reproduce the old `regs.pc=ffffffff` crash in pure L2
- after fencing it back off, a second remaining problem still prevents visible boot progress

### Diagnosis Path Forward

The highest-value next work is now:

1. fix the native short-BRA path properly
   - likely in constant-jump / block-follow / chain semantics rather than only the obvious displacement arithmetic
2. re-run pure-L2 / branch-family checks
3. only then re-evaluate the remaining black-screen / no-progress runtime stall

For the older broader async-isolation plan, see `docs/AARCH64_JIT_ISOLATION_MATRIX.md`.

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
9acb53b0 aarch64 jit: keep FLAGX in interpreter format in memory
144e8a4d aarch64 jit: implement legacy adc/sbb with carry semantics
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
