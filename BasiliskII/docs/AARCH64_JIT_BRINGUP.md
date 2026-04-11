# AArch64 JIT Bringup — BasiliskII on ARM64

## Overview

This document describes the work done to bring up the experimental AArch64 (ARM64) JIT compiler in BasiliskII, enabling native ARM64 code generation for m68k emulation. The JIT translates 68040 instructions into ARM64 native code at runtime, with two optimization levels:

- **L1 (optlev=1)**: All instructions fall through to interpreter with per-instruction spcflags checks. Safe but slow — no native codegen.
- **L2 (optlev=2)**: Instructions compile to native ARM64 code. Register allocator tracks values across instructions within a block. ~2-5× faster than L1.

The codebase is a fork of the Koenig/cebix/aranym JIT originally written for x86, ported to ARM (32-bit) by contributors, with an ARM64 backend added experimentally. Since the initial bringup, the work has fixed a long series of structural and semantic bugs: byte-order mismatches, IRQ deliverability, legacy carry/X handling, boundary cycle charging, verifier misuse, ARM64 endblock slow-path bugs, short-branch decode on ARM64, 32-bit host-PC construction bugs, word-to-address-register self-alias clobbers, A-line trap control-flow modeling, 64-bit pointer truncation in the legacy `add_l`/`sub_l_ri` helpers, the `endblock_pc_isconst` direct-chain stale-state bug, and EMUL_OP barrier requirements. **Pure L2 now runs at 93.75% ROM coverage + 100% RAM, with SCSIGet × 7 + SCSISelect × 7, zero crashes, zero `bad_pc_p`, and 634M dispatches/120s with direct `B` chaining.**

## Current status (2026-04-11)

### Interpreter baseline

The pure interpreter (`jit false`) boots Mac OS 7.5.5 to the Finder desktop in approximately 60 seconds on the Orange Pi 6 Plus (CIX P1 12-core ARM64). The disk image (HD200MB with System 7.5.5) is confirmed working. An "improper shutdown" dialog appears on first boot and requires a Return keypress to dismiss.

### JIT status

The ARM64 JIT **does not yet boot to desktop**. Two blocking issues remain:

1. **Heap corruption with MAXRUN=1 single-instruction blocks**: An L2-compiled ROM block writes `0x08` to address `0x2038` (the system heap's first block size field, should be `0x110`). This causes the Memory Manager's heap walk at `0x0400e1a4` to loop infinitely, blocking all boot progress past SCSI probe.

2. **Register state corruption without MAXRUN=1**: Multi-instruction blocks corrupt A7 (stack pointer), producing `a7=0xffffff22` (bogus) and crashing early boot. This prevents removing the MAXRUN=1 workaround.

With MAXRUN=1, the JIT reaches SCSIGet×7 + SCSISelect×7 (full SCSI bus probe) with zero crashes and 634M dispatches/120s, but does not progress to DiskStatus or beyond.

### L2 coverage

| Region | Coverage | Notes |
|---|---|---|
| ROM `0x04010000-0x040fffff` | **93.75% L2** | All non-init ROM code |
| ROM `0x04000000-0x0400ffff` | Interpreter | Low-ROM startup, `$dd0` I/O polling |
| RAM (Mac OS + applications) | **100% L2** | Full native ARM64 codegen |

### Key fixes — full commit history

All fixes during the April 2026 bringup sessions, in chronological order:

| Commit | Date | Fix |
|---|---|---|
| `adc83002` | Apr 10 | A-line trap L2 runtime helpers — `op_aline_trap_comp_ff` runs `Exception(0xA,0)` and ends block |
| `ef56ff56` | Apr 10 | Barrier bisection infrastructure — proved `branch` family (6xxx) is the divergence source |
| `91f2e0f8` | Apr 10 | 64-bit pointer truncation — `add_l`/`sub_l_ri` routed through `arm_ADD_l` for PC_P |
| `8c8d4f6d` | Apr 10 | Hot-chain `regs.pc_p` store + BSR barrier |
| `03c0c1cf` | Apr 10 | BSR dynamic exit — changed BSR to use dynamic exit instead of direct chaining |
| `6838db5d` | Apr 10 | Full PC triple on endblock — `endblock_pc_isconst` stores `pc_p`, `pc`, `pc_oldp` |
| `9297d0de` | Apr 10 | PC_P const validation guard at compile time |
| `f155eb38` | Apr 10 | Bringup doc update |
| `c59cf7fe` | Apr 10 | Spcflags mid-block PC triple — cold path stores full triple for `m68k_getpc()` |
| `24cc7373` | Apr 10 | EMUL_OP interpreter barrier |
| `80afe33d` | Apr 10 | Unconditional `regs.pc_p` store on hot chain |
| `35c02bcb` | Apr 10 | Narrow containment + EMUL_OP barrier |
| `7280caf4` | Apr 10 | NuBus read hook (experimental, later removed) |
| `0cbcee62` | Apr 10 | Restore 69% L2 config |
| `bcba9fe7` | Apr 10 | Remove NuBus hook |
| `bf9a569b` | Apr 10 | Restore full working boot config |
| `ee27ef35` | Apr 11 | ROM-patch NuBus video probe → 93.75% L2 |
| `1f692578` | Apr 11 | Signature-guarded NuBus patch + ROM fallback + docs |
| `ffb1b731` | Apr 11 | **Cross-block flag loss fix** — `flush(save_regs=1)` forces `flags_are_important=1` |
| `872ddd69` | Apr 11 | **Remove `tick_inhibit` during block tracing** — restores 60Hz timer cadence |
| `1f43f27f` | Apr 11 | **Mid-block tick injection** — every 64 compiled instructions, emit `cpu_do_check_ticks()` + spcflags check |

### ROM compatibility

The NuBus probe patch is guarded by a signature check at ROM offset `0xb27c`:
- **Quadra 800 ROM**: signature matches → patch applied → 93.75% L2
- **Other ROM32 ROMs**: signature may or may not match. If not, the `040b`/`0404` interpreter containment activates automatically → 62.5% L2
- **Classic/Plus ROMs**: different ROM version, JIT patches don't apply

### Remaining work

To boot Mac OS with the JIT:

1. **Fix register state propagation in multi-instruction blocks.** Without `MAXRUN=1`, compiled blocks corrupt A7 (stack pointer). The register allocator's entry-state assumptions from trace time don't hold when blocks are entered from different source blocks or after interrupt delivery. This is the primary blocker.

2. **Remove `MAXRUN=1` and test with mid-block tick injection.** Once multi-instruction blocks work, the `JIT_TICK_INTERVAL=64` tick injection ensures the 60Hz timer fires and interrupts are delivered mid-block. This has been implemented but is inactive with `MAXRUN=1`.

3. **Investigate heap corruption at `0x2038`.** With `MAXRUN=1`, an L2-compiled ROM block writes `0x08` to the system heap's first block size field. This may be a symptom of the same register state bug that corrupts A7 in larger blocks, or a separate codegen issue in a specific opcode handler.

4. **Performance tuning.** Once boot works, measure JIT vs interpreter speedup and optimize hot paths (direct block chaining, flag elimination, register allocation).

### Architecture

The endblock hot chain path on ARM64:
1. `flush(1)` writes all dirty/const registers including PC_P to memory
2. `endblock_pc_isconst` stores `regs.pc_p = v` (target host pointer)
3. Direct `B` to target block's handler via `create_jmpdep`
4. Target block runs with correct `regs.pc_p`; interpreter fallbacks rebuild the full PC triple before each `cputbl` call

Special opcode handling:
- **A-line (`0xAxxx`)**: L2 compiled runtime helper
- **EMUL_OP (`0x71xx`)**: Interpreter barrier (ends block, re-enters dispatcher)
- **All other opcodes**: Full L2 native ARM64 codegen

## Current frontier (2026-04-11)

The most important updates from the April 10-11 session:

### 1. A-line trap class moved into L2 (`adc83002`)

A-line opcodes (like `A995`) now execute through a dedicated L2 runtime helper (`op_aline_trap_comp_ff` → `jit_runtime_aline_trap`). This runs the real `op_illg()/Exception()` path and ends the block from the helper-updated `regs.pc_p`.

### 2. 64-bit pointer truncation fix (`91f2e0f8`)

`add_l(d, s)` and `add_l_ri(d, i)` routed through `jnf_ADD_l` which uses 32-bit `ADD Wd,Wd,Ws`. When `d` is `PC_P` (a 64-bit host pointer), this truncated the upper bits. Fixed by detecting `d == PC_P` and routing through `arm_ADD_l`/`arm_ADD_l_ri` which use 64-bit arithmetic with proper sign-extension. Same fix applied to `sub_l_ri`.

### 3. Barrier bisection identified the branch family (`ef56ff56`)

Systematic bisection of interpreter barrier families proved:
- `B2_JIT_RESTORE_BARRIERS=branch` alone restored boot progress (`e1b2` × 20)
- Within `branch`: `bsr` alone eliminated the late hardware loop
- `bcc` alone also partially helped
- The root cause was in the block-exit chaining path, not the branch instruction semantics

### 4. `endblock_pc_isconst` direct-chain bug identified and fixed (`6838db5d`, `9297d0de`)

**Root cause**: `endblock_pc_isconst`'s hot-chain path emitted a direct `B` instruction to the target block's handler, bypassing the block-entry validation that `execute_normal()` provides. When blocks are directly chained, the target block can run with:

1. Stale `regs.pc` / `regs.pc_oldp` (the PC triple was only partially updated)
2. Invalid block state (checksum changes, recompilation, state mismatches)

**Evidence**:
- Compile-time `PC_P` values are always valid (zero `JIT_BAD_CONST` hits)
- `B2_JIT_FLUSH_EACH_OP=1` proved the issue is in the endblock path, not instruction semantics
- Replacing direct `B` chain with `execute_normal` re-entry: `e1b2` × 284-304
- With direct `B` chain + PC triple store: still fails

**Fix**: The hot path now stores the full PC triple (`regs.pc_p`, `regs.pc`, `regs.pc_oldp`) and re-enters `execute_normal()` via `raw_pop_preserved_regs + jmp`. This ensures proper block validation on every transition.

### 5. Current pure-L2 status (no barriers, no optlev0 ranges)

| Metric | Before session | After fixes |
|---|---|---|
| `0x0400e1b2` | 0 | **284** |
| Late hardware loop (`0x04007116`) | 340k+ | **864k** (residual) |
| Hardware dead end (`0x0400706a`) | yes | **0** |
| Crash/SIGSEGV | occasional | **0** |
| `SCSIGet` | 0 | 0 |
| Dispatches/60s | stuck | **1M** |

### 6. Performance path forward

Every block exit now goes through the full C dispatcher (`execute_normal`), which is ~100× slower than direct chaining. To restore performance:

1. Use the target block's **non-direct handler** (`handler_to_use` instead of `direct_handler_to_use`) for direct chains — this includes checksum validation
2. Or fix the underlying state propagation so direct chains are safe

### 7. Residual late hardware loop

The `0x04007116` polling loop (864k hits alongside 284 `e1b2` hits) is a separate issue from the endblock chaining bug. It's the same `0x50f...` hardware-space polling path identified on April 10, entered through a different feeder than the one eliminated by the A-line and pointer fixes.

## Previous frontier (2026-04-10)
2. run the real `Exception(0xA, 0)` path
3. end the block on the helper-updated `regs.pc_p`
4. re-enter dispatch from the trap-established machine state

This preserved the first-principles semantics while removing the earlier interpreter-only containment.

### 2. The old `A995` startup blocker is gone in pure L2

Post-change pure-L2 smokes now clearly get past the former frontier:

- `A995` still executes at `0x040011e4`
- pure L2 later reaches:
  - `0x04000266`
  - `0x04009500`
  - `0x04001396`
- pure L2 also still reaches the later frontier around:
  - `0x04007118`

So the remaining failure is no longer the old `A995` return-path issue.

### 3. The next frontier is a later ROM wait/copy routine, not `DBcc` or `TST` semantics themselves

The strongest current evidence comes from dispatch tracing around:

- `0x0400706a..0x04007124`

Pure L2 enters this routine with:

- `a3 = 0x50f14000`
- `a5 = 0x50f00000`
- `m[a5] = 0xff`

and then runs the inner wait loop:

- `0x04007116: 4a15`
- `0x04007118: 51cc fffc`

while polling `m[a5]`, which remains `0xff`.

Important point: the local loop mechanics are behaving coherently.

Observed in trace:

- `d4` is loaded from `d0` and decrements normally through the `0x04007118` `DBcc`
- the loop then repeats because the polled value at `a5` never changes

So the current late failure is **not** explained by a direct `DBcc` decrement bug or a local `TST.B (A5)` bug.

### 4. Current best causal reading

Pure L2 is reaching a later ROM path that behaves like a hardware/video/slot wait-and-copy sequence and then spins because its polled byte never changes:

- `m[a5] == 0xff` throughout the loop
- the routine keeps using the `0x50f00000 / 0x50f14000` address family

The best current interpretation is:

- the all-native run is now diverging into the wrong later ROM service path or carrying the wrong service state into it
- once there, it waits on a slot/video-style status location that never becomes ready in the pure-L2 execution

That means the next bug is **earlier state/control selection feeding this loop**, not the inner `DBcc` / `TST` pair itself.

### 5. Status

- old `A995` trap-return blocker: **fixed in L2**
- pure L2 no longer depends on the old A-line interpreter containments
- boot still **not fixed** all-native
- current late frontier: `0x0400706a..0x04007124`, with the observed spin centered on polling `m[a5] == 0xff`

## Previous frontier (2026-04-09)

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

### 2. ARM64 endblock and host-PC corruption bugs were real and are now fixed

Multiple concrete ARM64 contract bugs were identified and fixed in dispatcher/chain handoff and target construction paths:

1. **setpc trampolines only updated `regs.pc_p`**
   - `popall_execute_normal_setpc`
   - `popall_check_checksum_setpc`
   - `popall_exec_nostats_setpc`

   These now rebuild canonical PC state instead of entering C/interpreter code with stale `regs.pc` / `regs.pc_oldp`.

2. **ARM64 endblock helpers used brittle negative-branch skip counts**
   - `compemu_raw_endblock_pc_inreg()`
   - `compemu_raw_endblock_pc_isconst()`

   These were rewritten to use explicit patched hot/slow branch layout, removing the hard-coded skip-count contract.

3. **legacy generated code could build host PCs with a bogus `+4GB` bias**
   - some branch/jump paths still formed host-PC targets through 32-bit signed temporaries before adding `comp_pc_p`
   - representative bad host targets included `0x114000488`, `0x11400091e`, `0x1140b3494`

   This was fixed by using pointer-width addition paths and strengthening `arm_ADD_l_ri(...)` to treat low-32-bit pointer immediates as pointer bases with sign-extended displacements.

These fixes removed a real class of `bad pc_p` / `exec_normal bad` / host-target corruption failures from the safe runtime.

### 3. The old short-BRA crash family is no longer the main frontier

Barrier binary search originally localized the immediate pure-L2 crash family to native short BRA (`BRAQ.B`). That work produced two real fixes:

- short `BRA/BSR/Bcc` 8-bit displacement decode on ARM64 was corrected to use the low opcode byte under `HAVE_GET_WORD_UNSWAPPED`
- the const-target short-BRA exit path was changed to write guest PC and re-enter through `compemu_raw_execute_normal_cycles(...)`

After those fixes and the host-PC repair, pure-L2 no longer shows the old corruption signature in current smokes:

- `bad pc_p = 0`
- `exec_normal bad = 0`
- `SIGSEGV = 0`
- `SIGBUS = 0`

### 4. The frontier moved from the old `0x04009ab0` loop hypothesis to a much smaller ROM startup corridor

Pure L2 still does not boot, but the failure mode has narrowed substantially.

Current visual / boot metrics in the failing all-native path remain:

- Xvfb visual checks black
- `DiskStatus = 0`
- `SCSIGet = 0`
- `SDL_VIDEOINT = 0`
- `SDL_PRESENT = 0`

Optlev-0 range narrowing now shows that meaningful progress is recovered only when a very small set of ROM regions is interpreter-forced:

- always-needed base containments:
  - `0x04000000-0x0400ffff`
  - `0x04040000-0x0407ffff`
  - `0x040b0000-0x040bffff`
- narrowed low-side cluster:
  - `0x0401b6d4-0x0401b6de`
- narrowed high-side cluster:
  - `0x0401be46-0x0401be94`

That high-side narrowing moved the best current frontier away from the older `0x04009ab0..0x04009ad8` table-builder hypothesis and into the later ROM startup corridor around:

- `0x0401b6d4..0x0401b6de`
- `0x0401be46..0x0401be94`
- especially the dynamic block rooted at `0x0401be94`

### 5. A real native codegen bug was found and fixed at `0x0401be8a`

Inside that narrowed high-side range, the following ROM instruction was confirmed to have a real compiled self-alias bug:

- `0x0401be8a: adda.w $1a(a1), a1`

This is the generic `ADDA.W (d16,An),An` case with `srcreg == dstreg`.

**Root cause**:

- the classic generator order materialized the destination `An` before preserving the loaded word source
- in the self-alias case, that allowed host-register reuse to clobber the loaded source before `sign_extend_16_rr(...)`

**Fix**:

- changed the classic `ADDA` / `SUBA` generators in `gencomp.c` and `gencomp_arm.c` so the source is sign-extended into a stable temporary **before** destination materialization
- rebuilt generated handlers in `src/Unix/compemu.cpp`

Single-op native disassembly after the fix shows the correct sequence for `0x0401be8a`: load word, byte-swap, sign-extend to a stable temp, reload original `A1`, then add `A1 + signext(word)`.

### 6. That fix was real, but it was not the last blocker

Post-fix smokes confirm that the `0x0401be8a` bug was genuine, but boot still does not progress in all-native mode.

In particular, after the fix:

- the base-only all-native smoke still stalls with `CHECKLOAD = 0`, `IRQ = 0`
- base + low-side `0x0401b6d4-0x0401b6de` also still stalls with `CHECKLOAD = 0`, `IRQ = 0`

So the self-alias producer at `0x0401be8a` was one real codegen bug, but not the last remaining native semantic mismatch in the narrowed corridor.

### 7. A second real codegen bug was then found inside the `0x0401be94` call/callee segment

After fixing `0x0401be8a`, targeted block verification on the full block rooted at `0x0401be94` still found a deterministic stack-memory mismatch.

The culprit was the inlined callee store path for:

- `0x0401bfd0: movem.l d0/a0-a1, -(a7)`

More specifically, the ARM64 legacy helper `mov_l_Rr(d, s, offset)` could use the same work register for:

- the source value being stored
- and the temporary address calculation for a negative offset

That clobbered the source before the store, so the native code wrote a host-pointer-shaped value to the stack slot instead of the intended big-endian longword.

**Fix**:

- taught `legacy_addr_with_offset(...)` to avoid the source register when selecting its temporary
- updated `mov_l_Rr()` / `mov_w_Rr()` to request an address temp that cannot alias the source register

After that fix, repeated block verification for:

- `JITBLOCKVERIFY block=0401be94 len=16`

went from `mismatch=1` to repeated `mismatch=0`.

### 8. The reduced working workaround now isolates two exact high-side PCs

With the `0x0401be94` body fixed and verifying clean, the previously broad high-side workaround narrows further.

The current reduced working combination is:

- base containments:
  - `0x04000000-0x0400ffff`
  - `0x04040000-0x0407ffff`
  - `0x040b0000-0x040bffff`
- low-side:
  - `0x0401b6d4-0x0401b6de`
- exact high-side PCs:
  - `0x0401be4c`
  - `0x0401be88`

That reduced combo still restores meaningful progress (`CHECKLOAD`, `IRQ`, `intmask -> 0`).

### 9. Exact opcode checks for `0x0401be4c` and `0x0401be88` are clean

The two exact high-side PCs are:

- `0x0401be4c: bsr.w $401b698`
- `0x0401be88: movea.l (a4), a1`

Both were checked with the exact per-instruction verifier (`B2_JIT_VERIFY_PCS=...`). In both cases:

- the opcode body compiled
- no `JITVERIFY` mismatch line was emitted

So neither exact instruction shows a direct compiled-vs-interpreter mismatch at the single-instruction level.

### 10. But block verification at those PCs still mismatches

Despite the exact per-instruction verifier being clean, block verification still reports mismatches for blocks rooted at those same PCs.

Representative results:

- `0x0401be4c` one-op block: exact instruction verifies clean, but block verification can still mismatch on exit PC / flags / stack state
- `0x0401be88` one-op and short multi-op blocks: exact instruction verifies clean, but block verification still mismatches on exit PC / flags / stack bytes

This means the remaining bug is no longer best described as the isolated opcode semantics of `BSR.W` or `MOVEA.L (A4),A1` themselves.

### 11. Substitution checks also stayed narrow

Replacing the exact PCs with nearby downstream sites did **not** reproduce the workaround.

Examples that did **not** substitute for the exact sites:

- replacing `0x0401be4c` with callee/nearby sites such as `0x0401b698..0x0401b69a` or `0x0401b6d0..0x0401b6de`
- replacing `0x0401be88` with `0x0401be8a`, `0x0401be8e`, or `0x0401be90`

So the reduced workaround remains pinned to the exact PCs `0x0401be4c` and `0x0401be88`, even though the exact opcode bodies themselves verify clean.

### 12. Diagnosis path forward

The highest-value next work is now:

1. keep the confirmed `ADDA` / `SUBA` self-alias fix and the `mov_*_Rr` negative-offset fix committed and documented
2. compare native vs interpreter state at block exit / successor setup for the exact-PC workaround sites `0x0401be4c` and `0x0401be88`
3. inspect the block-end / successor-state mechanics around those two exact PCs, since that is where block verification still disagrees even when per-instruction verification does not
4. only after that re-evaluate whether any broader corridor hypothesis is still needed

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

### Bug 16: Word-to-An Self-Alias Source Clobber in Classic `ADDA`/`SUBA` Generation (2026-04-09)

**Symptom**: After narrowing the remaining all-native failure with targeted `optlev=0` ranges, the ROM instruction

- `0x0401be8a: adda.w $1a(a1), a1`

was identified as a real compiled-code producer bug inside the smallest failing high-side range `0x0401be46-0x0401be94`.

**Root cause**: In the classic non-`USE_JIT2` generators, `ADDA` / `SUBA` generated the destination address-register path before preserving the loaded source word. In self-alias forms (`srcreg == dstreg`), that allowed host-register reuse to clobber the loaded source before `sign_extend_16_rr(...)` copied it into a stable temporary.

**Fix**:

- changed `gencomp.c` and `gencomp_arm.c` so `ADDA` / `SUBA` sign-extend/copy the source into `tmp` **before** destination materialization
- rebuilt the generated handlers in `src/Unix/compemu.cpp`

This fixes the real self-alias codegen bug at `0x0401be8a`, although boot still remained blocked by a deeper mismatch later in the `0x0401be94 -> 0x0401bfd0..0x0401bfde` path.

### Bug 17: Negative-Offset Store Temp Alias in ARM64 Legacy `mov_*_Rr` Helpers (2026-04-09)

**Symptom**: Targeted block verification for the block rooted at `0x0401be94` still showed a deterministic stack-memory mismatch after bug 16 was fixed. The mismatch came from the inlined callee path for:

- `0x0401bfd0: movem.l d0/a0-a1, -(a7)`

where native code wrote a host-pointer-shaped value into the saved-stack slot instead of the intended big-endian longword.

**Root cause**: In `compemu_legacy_arm64_compat.cpp`, the legacy helper `mov_l_Rr(d, s, offset)` used a temporary chosen by `legacy_addr_with_offset(...)`. For negative offsets, that temp could alias the source register itself, so address formation clobbered the value that was supposed to be stored.

**Fix**:

- introduced an address-temp selection helper that can avoid a specified register
- updated `mov_l_Rr()` and `mov_w_Rr()` to forbid aliasing the source register during offset-address formation

This made repeated `JITBLOCKVERIFY block=0401be94 len=16` runs compare clean.

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
- The real self-alias `ADDA` / `SUBA` source-clobber bug in classic word-to-`An` generation is now fixed.
- The negative-offset temp-alias store bug in ARM64 legacy `mov_*_Rr` helpers is now fixed, and the full `0x0401be94` block now verifies clean.

### Remaining Issue

Boot still does **not** reach visible Mac OS progress in the current safe configuration:

- Xvfb capture remains black
- `DiskStatus=0`
- `SCSIGet=0`
- `SDL_VIDEOINT=0`
- `SDL_PRESENT=0`

But the nature of the failure is now more specific:

1. the old pure-L2 `bad pc_p` / `exec_normal bad` crash family is no longer the dominant symptom
2. the most useful current `optlev=0` narrowing points to the ROM startup corridor around `0x0401b6d4..0x0401be94`
3. the confirmed `0x0401be8a` self-alias bug was real, and a second real store-temp alias bug inside the `0x0401be94` callee path was also real and is now fixed, but boot still does **not** progress
4. the current reduced working workaround now isolates the exact PCs `0x0401be4c` and `0x0401be88`, even though exact per-instruction verification shows those opcode bodies match the interpreter

### Most useful current interpretation

The old broad “async/timing explains everything” hypothesis is no longer the best summary.

The strongest current picture is:

- a real native short-branch / host-PC corruption bug existed on ARM64 L2 and has been repaired enough to keep pure L2 stable
- a second real codegen bug existed in classic self-alias `ADDA` / `SUBA` word-to-`An` lowering and is now fixed
- a third real bug existed in ARM64 legacy negative-offset store address formation (`mov_*_Rr`) and is now fixed
- the full block rooted at `0x0401be94` now verifies clean, so the next blocker is elsewhere in the narrowed corridor
- the remaining disagreement is now concentrated at block-exit / successor-state behavior around the exact workaround PCs `0x0401be4c` and `0x0401be88`, not in their isolated opcode bodies

### Diagnosis Path Forward

The highest-value next work is now:

1. compare native vs interpreter state across block exit / successor setup for the exact workaround PCs `0x0401be4c` and `0x0401be88`
2. treat the isolated opcode bodies there as provisionally verified-clean and focus on why block verification still diverges at those roots
3. repair the next real native mismatch in that path
4. only after that re-evaluate whether any of the older broader loop hypotheses still matter

For the updated experiment framing, see `docs/AARCH64_JIT_ISOLATION_MATRIX.md`.

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
