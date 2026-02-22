# JIT Video Corruption — Root Cause Analysis

**Branch**: `feature/arm-jit`  
**Date**: 2026-02-08  
**Target**: Raspberry Pi 3B (Cortex-A53, ARMv8-A running ARMv7 armhf), KMSDRM + OpenGL ES 2.0  
**Symptom**: Patterned screen corruption, worse at lower bit depths, with JIT enabled

---

## Progress Update (2026-02-22)

This document remains valid for the original 2026-02-08 race/corruption analysis, but debugging since then established additional, concrete crash causes and mitigation status.

### Confirmed Since Initial Report

1. **JIT activation path was initially disabled by prefs defaulting** (fixed).
2. **ARM icache coherency bug at JIT popall stub generation** caused immediate compiled-entry crashes (fixed by explicit `flush_cpu_icache` after code emission).
3. **Unformatted JIT logs** made crash sequencing hard to read (fixed by newline-safe `jit_log` behavior).
4. **Current primary corruption/crash chain** is reproducibly:
     - opcode handler `op_2068_0_ff` loads A3 from an out-of-range guest source address,
     - loaded value becomes `0x50f14000` after endian conversion,
     - later opcode `op_4a28_0_ff` dereferences via A3 and faults.

### GDB-Proven Address Evidence

- RAM bounds at runtime: `RAMBaseMac = 0x00000000`, `RAMSize = 0x08800000`, RAM end `0x08800000`.
- Fault-seeding source read observed in `op_2068_0_ff`: guest source around `0x088D36EC` (**outside RAM**).
- Bad loaded longword observed at source: `0x0040f150` (then swapped into `0x50f14000`).

### Mitigations Implemented So Far

- JIT exception trap path for Basilisk builds to convert host faults to exception flow instead of instant hard segfault.
- Repeated bus-error loop detector with automatic JIT fallback to interpreter execution.
- Direct-address read-side bounds checks added in memory accessors (`get_long/word/byte`) to catch invalid guest reads earlier.

### Regression Discovered and Corrected

- Throwing from broad write/pointer-conversion paths (`put_*`, `get_real_address*`) broke startup ROM patching (`PatchROM`) via uncaught `m68k_exception`.
- Corrective adjustment: keep strict read checks; avoid throw behavior in generic pointer conversion and ROM patch write paths.

### Current Status

- Original concurrency findings in this document are still relevant.
- A distinct, concrete invalid-guest-address read path is now confirmed and must be treated as a first-class root cause for current crashes.
- Validation on latest build should focus on:
    1. no uncaught `m68k_exception` during startup,
    2. no A3 transition to `0x5xxxxxxx` from out-of-range guest reads,
    3. stable interpreter fallback if repeated exception loops occur.

### Targeted Audit (Likely Fault Paths, 2026-02-22)

This is a focused static audit of the code paths most likely to still generate invalid host-memory dereferences under JIT.

1. **Basilisk JIT fast memory path is still effectively "trust guest address"**
    - `compiler/compemu_support.cpp` has Basilisk-path `#define canbang 1` and direct host accesses in `readmem_real()` / `writemem_real()` / `get_n_addr()`.
    - These paths emit native loads/stores against `MEMBaseDiff + guest_addr` with no range predicate in the emitted sequence.
    - If a guest EA is out of mapped Mac ranges but still host-mapped, values can be silently read/written instead of faulting immediately.

2. **Uncompiled opcode fallback runs inside JIT loop and depends on memory accessor semantics**
    - In block execution, compile failure routes to `cputbl[opcode]` (`compemu_support.cpp`) rather than a compiled handler.
    - Recent traces (`op_2068_0_ff`, `op_4a28_0_ff`) are consistent with this path being active for at least part of the failing sequence.
    - This makes `memory.h` `get_*`/`put_*` behavior a first-order correctness dependency even when "JIT enabled" is true.

3. **Exception-frame write path can still hard-fault if A7 is already corrupt**
    - `Exception()` builds frames via `exc_push_long()` -> `put_long()`.
    - If bus-error handling is entered with invalid SP, host write faults can occur while constructing the frame.
    - Current mitigation is JIT-side exception-2 immediate fallback before `Exception()`; this is a containment strategy, not a full invariant restore.

4. **EA calculators (`get_disp_ea_000/020`) are arithmetically correct but unchecked by design**
    - They propagate register-derived addresses without boundary checks.
    - Once a register is poisoned, downstream EA users amplify the fault quickly.

#### Prioritized Fix Candidates

1. **Safety-first containment (smallest change):** keep immediate JIT fallback on exception 2 and avoid re-entering `Exception()` from JIT catch path.
2. **Strengthen write-side safety in direct memory helpers:** reintroduce `put_*` bounds checks, but allow ROM writes needed by ROM patching (to avoid startup regression).
3. **Longer-term robust fix:** add an explicit Basilisk-side range guard before `canbang` direct memory emission for JIT-generated reads/writes (or force non-`canbang` path for risky regions).
4. **Diagnostics improvement:** log opcode + EA at first `THROW(2)` in `get_*` to identify first poison site without multi-step watchpoint setup.

### Latest Probe Update (2026-02-22, headless GDB)

Using `SDL_VIDEODRIVER=dummy` with `gdb -batch` and a conditional hardware watchpoint on `regs.regs[11]`:

- **Watchpoint condition:** `regs.regs[11] in [0x50000000, 0x50ffffff]`
- **First trigger:** `op_2068_0_ff` at `cpuemu.cpp:13793`
- **State transition:** `regs.regs[11]` changed from `0x08800000` to `0x50f14000`
- **Instruction context:** value loaded via `get_long(...)` then stored into A-register slot (`str.w r0, [r5, r4, lsl #2]`)

This reconfirms the previously identified root chain:

1. `op_2068_0_ff` seeds poisoned A3 from out-of-range read source,
2. later `op_4a28_0_ff` consumes that poisoned EA,
3. range checks throw exception 2 in memory helpers.

Additional observation from this build: traces may symbolize the throw site as `put_long()` (`memory.h:122`) due to optimization/inlining, but register/disassembly context still points to the same `op_4a28_0_ff` invalid-EA flow.

#### Debug-build policy (temporary)

For ongoing triage, keep ARMhf artifacts built with full symbols (and minimal inlining where practical) to preserve argument visibility in throw/bus-error frames and reduce ambiguity in inline helper attribution.

CI has been updated accordingly (`build-arm-jit.yml`) to use debug-friendly flags during configure/build:

- `-O0 -g3 -fno-omit-frame-pointer -fno-inline`
- linker build-id enabled (`-Wl,--build-id`)

---

## Comprehensive JIT Engine Audit (2026-02-22)

### Scope & Methodology

End-to-end static audit of the entire ARM JIT compilation pipeline covering:
- Initialization and configuration (`compemu_support.cpp`, `basilisk_glue.cpp`)
- Code generation pipeline (`gencomp.c` → `compemu.h`)
- ARM backend code emission (`codegen_arm.cpp`, `codegen_arm.h`)
- Mid-level functions (`compemu_midfunc_arm.cpp`, `compemu_midfunc_arm2.cpp`)
- Block cache management and execution loop (`compemu_support.cpp`, `newcpu.cpp`)
- Memory access architecture (`memory.h`, `memory.cpp`, `cpu_emulation.h`)
- Exception handling (`newcpu.cpp`, `registers.h`)

All line references are relative to the `feature/arm-jit` branch.

---

### A. Architecture Overview

```
                     ┌───────────────────────────────────────────┐
                     │          68k Instruction Stream           │
                     └───────────────┬───────────────────────────┘
                                     │
                          ┌──────────▼──────────┐
                          │   compile_block()    │
                          │  compemu_support.cpp │
                          └──────────┬──────────┘
                                     │
              ┌──────────────────────┼──────────────────────┐
              │ For each 68k opcode: │                      │
              │                      │                      │
    ┌─────────▼─────────┐  ┌────────▼────────┐   ┌────────▼────────┐
    │  comptbl[opcode]   │  │ failure → call  │   │ optlev 0: just  │
    │ (gencomp-generated)│  │ cputbl[opcode]  │   │ jump to interp  │
    │                    │  │ (interpreter)   │   │ (exec_nostats)   │
    └─────────┬──────────┘  └────────┬────────┘   └─────────────────┘
              │                      │
    ┌─────────▼──────────┐           │
    │   Mid-functions     │           │
    │ compemu_midfunc_arm │           │
    │  (readlong, etc.)   │           │
    └─────────┬──────────┘           │
              │                      │
    ┌─────────▼──────────┐           │
    │  codegen_arm.cpp    │           │
    │  (ARM instruction   │           │
    │   emission)         │           │
    └─────────┬──────────┘           │
              │                      │
              │  Native ARM code     │  C function call
              ▼                      ▼
    ┌─────────────────────────────────────────────┐
    │              Host RAM via MEMBaseDiff        │
    │  LDR/STR [MEMBaseDiff + guest_addr]         │
    │              *** NO BOUNDS CHECK ***         │
    └─────────────────────────────────────────────┘
```

Two paths exist for memory access at runtime:

| Path | Bounds Check | Result on Bad Address |
|------|-------------|----------------------|
| **Interpreter** (`get_long()` etc. in `memory.h`) | ✅ `is_direct_address_valid()` → `THROW(2)` | Proper 68k bus error |
| **JIT native** (`readmem_real()` → ARM `LDR`) | ❌ None | Garbage if mapped, SIGSEGV if unmapped |

---

### B. JIT Initialization & Configuration

#### B.1 `canbang` — The Root Design Decision

**`compemu_support.cpp:201-202`**:
```cpp
#define NATMEM_OFFSET MEMBaseDiff
#define canbang 1
```

In the BasiliskII path (non-UAE), `canbang` is a **compile-time constant `1`**. This unconditionally enables the "direct bang" memory path in all JIT codegen. The UAE path has `canbang` as an `extern bool` that can be toggled at runtime.

**Impact**: Every `readmem_real()`, `writemem_real()`, and `get_n_addr()` call in the JIT emits direct memory access with zero bounds checking. This is the single most consequential design decision in the entire JIT.

#### B.2 MEMBaseDiff Calculation

**`main_unix.cpp:879`** (Unix init):
```cpp
MEMBaseDiff = (uintptr)RAMBaseHost;
```

**`basilisk_glue.cpp:82-102`** (DIRECT_ADDRESSING path):
```cpp
RAMBaseMac = 0;
ROMBaseMac = Host2MacAddr(ROMBaseHost);  // = RAMSize (e.g. 0x08800000)
```

Address translation: `host_ptr = MEMBaseDiff + mac_addr`

| Region | Mac Address | Host Address | Calculation |
|--------|------------|-------------|-------------|
| RAM | `0x00000000` – `RAMSize` | `RAMBaseHost` – `RAMBaseHost+RAMSize` | Directly mapped ✅ |
| ROM | `RAMSize` – `RAMSize+ROMSize` | `RAMBaseHost+RAMSize` – ... | Directly mapped ✅ |
| Frame buffer | `Host2MacAddr(the_buffer)` | `the_buffer` | Depends on mmap ⚠️ |

**ARM32 Assessment**: All pointers are 32-bit, `uintptr` = `uint32`. The `assert((uintptr)ptr <= 0xffffffff)` in `alloc_code()` is tautologically true. No 64-bit → 32-bit truncation risks exist. The modular arithmetic of `Host2MacAddr(the_buffer) = the_buffer - RAMBaseHost` wraps correctly even if the buffer is at a lower address.

#### B.3 Frame Buffer Address Bug in `is_direct_address_valid()`

**`memory.h:65-82`** — the interpreter's bounds check:
```cpp
#if !REAL_ADDRESSING
    const uae_u32 frame_base = 0xa0000000;    // HARDCODED
    if (addr >= frame_base && end < frame_base + MacFrameSize) return true;
#endif
```

**BUG**: The hardcoded `0xa0000000` is only correct for the non-DIRECT_ADDRESSING bank-based path. In DIRECT_ADDRESSING mode (which the JIT build uses), the Mac frame buffer address is `Host2MacAddr(the_buffer)`, which is dynamically computed and is **not** `0xa0000000`.

**Impact**: Interpreter accesses to the actual framebuffer Mac address could incorrectly throw bus errors (THROW(2)) since the address won't match the hardcoded range. The JIT bypasses this check entirely, so it's a latent interpreter-path bug rather than the JIT crash root cause. However, this could cause interpreter fallback code to throw unexpected exceptions for legitimate framebuffer operations.

#### B.4 Compiler Initialization Sequence

**`build_comp()` at `compemu_support.cpp:4462-4610`**:
1. `raw_init_cpu()` — ARM CPU feature detection
2. `install_exception_handler()` — SIGSEGV/SIGBUS handler for JIT fault recovery
3. Build `compfunctbl[65536]` + `nfcompfunctbl[65536]` — opcode → compiler function mapping
4. `create_popalls()` — Generate entry/exit stubs (DMB barriers, register save/restore)
5. `alloc_cache()` — Allocate the translation cache (RWX memory)
6. Initialize `cache_tags[65536]` — all point to `popall_execute_normal`

CPU level mapping for Quadra 800 (`CPUType=4`): cpu_level=4 (68040), all 68040 opcodes enabled. Correct.

#### B.5 Cache Allocation

**`alloc_cache()` at `compemu_support.cpp:3786-3825`**:
```cpp
while (!compiled_code && cache_size) {
    if ((compiled_code = alloc_code(cache_size * 1024)) == NULL)
        cache_size /= 2;    // Halve and retry
}
vm_protect(compiled_code, cache_size * 1024,
           VM_PAGE_READ | VM_PAGE_WRITE | VM_PAGE_EXECUTE);
```

Cache overflow triggers a full flush (`flush_icache_hard()`). No incremental GC or LRU eviction exists — standard for this JIT lineage but means self-modifying code patterns cause frequent full flushes.

---

### C. Code Generation Pipeline

#### C.1 Opcode Compilation Flow (gencomp.c)

`gencomp.c` is a **code generator generator**: it produces `compemu.cpp` at build time, which contains one `op_XXXX_comp_ff()` function per compilable opcode. Each function calls mid-level helper functions to emit ARM native code.

For `op_2068` (MOVEA.L d16(An), An) — the known-buggy opcode:

```
gencomp.c generates:
  ① mov_l_rr(srca, 8 + srcreg)           // Copy An to scratch
  ② lea_l_brr(srca, srca, (s32)(s16)d16) // srca = An + sign_ext(d16)
  ③ readlong(srca, src, scratchie)        // src = mem[srca] — NO CHECK
  ④ mov_l_rr(dst, src)                    // dst = loaded value
  ⑤ mov_l_rr(dstreg + 8, dst)            // An = dst (store back)
```

**The `dodgy` mechanism** correctly handles self-referencing cases (e.g., `MOVEA.L d16(A0), A0`) by using a scratch register for the destination, avoiding aliasing with the source register.

**Sign extension** of the 16-bit displacement is correct: `(uae_s32)(uae_s16)comp_get_iword(...)` at compile time, baked into the generated ARM code as a literal constant.

#### C.2 JIT vs Interpreter Comparison for op_2068

| Aspect | JIT | Interpreter | Match? |
|--------|-----|-------------|--------|
| Address computation | `An + sign_extend_16(d16)` | Same | ✅ |
| Memory read | `readlong()` → `LDR [MEMBaseDiff+EA]` | `get_long()` → bounds check → LDR | **❌ No bounds check** |
| Byte swap | `mid_bswap_32()` (REV) | `do_get_mem_long()` | ✅ |
| MOVEA.L semantics | `mov_l_rr` (straight copy) | `val = src` | ✅ |
| MOVEA.W sign extend | `sign_extend_16_rr` (SXTH) | `(uae_s32)(uae_s16)` | ✅ |
| Condition codes | Not touched | Not touched | ✅ |

**The opcode generation is semantically correct.** The bug is not in MOVEA.L compilation — it's in the memory access path that lacks bounds checking.

#### C.3 Compilability Criteria

Opcodes that **always fail** (fall back to interpreter):
- Privileged instructions (plev ≥ 2)
- BCD: `SBCD`, `ABCD`, `NBCD`
- SR/USP manipulation: `ORSR`, `EORSR`, `ANDSR`, `MV2SR`, etc.
- Division: `DIVU`, `DIVS`, `DIVL`
- Bit field: all `BF*` variants
- Extended rotate: `ROXL`, `ROXR`
- System: `RESET`, `STOP`, `RTE`, `RTR`, `TRAPV`
- MMU ops, `CAS`, `CAS2`, `MOVES`, `TAS`, `PACK`, `UNPK`
- Bcc conditions VS/VC (conditions 8/9)

**MOVE and MOVEA are always compilable** — the `DISABLE_I_MOVE`/`DISABLE_I_MOVEA` defines are inside a commented-out block.

#### C.4 Dead Code: `compemu_midfunc_arm2.cpp` / `compemu_midfunc_arm2.h`

**`USE_JIT2` is never defined anywhere in the project.** The inclusion guard:
```cpp
#if defined(USE_JIT2)
#include "compemu_midfunc_arm2.cpp"
#endif
```
means the entire `arm2` file — all `jnf_MOVEA_l`, `jnf_MOVE`, `jff_ADD_*`, etc. — is **never compiled**. These were part of ARAnyM's JIT v2 refactoring and require a different front-end than `gencomp.c`. The only active mid-functions are in `compemu_midfunc_arm.cpp`.

#### C.5 Dead Scratch Register in i_MOVEA

**`gencomp.c:1700`**:
```c
comprintf("\tint tmps=scratchie++;\n");    // Allocated but NEVER used
```
Wastes one scratch register per MOVEA compilation. Minor inefficiency.

---

### D. ARM Backend (codegen_arm.cpp / codegen_arm.h)

#### D.1 IMM32 Rotation Macro — Silent Zero on Failure

**`codegen_arm.h:70-87`** — The `IMM32(c)` macro encodes a 32-bit constant as ARM's "8-bit rotated immediate" (imm8 ROR 2×rot). It checks 16 rotation positions. **If none match, it silently returns `0`**:

```c
#define IMM32(c) (((c) & 0xffffff00) == 0 ? (c) : \
                  /* ... 15 more rotation checks ... */ \
                  ((c) & 0xfffffc03) == 0 ? (0xf00 | ((c) >> 2)) : \
                        0)    // ← SILENT ZERO on failure
```

No assertion, no error. Any instruction using a non-representable immediate silently operates on `#0`. In practice the codegen uses representable constants (0xFF, 0xFF00, etc.), but this is a latent corruption risk for any future changes.

#### D.2 Register Allocation — Severe Pressure

| Register | Usage | Available for m68k? |
|----------|-------|-------------------|
| R0-R1 | Params, return, MUL results | Caller-saved scratch |
| R2-R3 | `REG_WORK1`/`REG_WORK2` — **permanently reserved** | **No** |
| R4-R12 | Callee-saved | **Yes** (9 registers) |
| R13 | SP | No |
| R14 | LR | No |
| R15 | PC | No |

Only **9 GPRs** to map **16 m68k registers** (D0-D7, A0-A7), guaranteeing frequent spilling. The two permanently reserved scratch registers are necessary because ARM multi-instruction sequences (e.g., carry inversion, byte-ops) need temporaries.

#### D.3 Byte Swapping — Correct

With `ARMV6_ASSEMBLY` defined (Raspberry Pi 3):
- 32-bit: `REV Rd, Rs` — single instruction ✅
- 16-bit: `REVSH`+`UXTH` — correct with upper-half preservation ✅
- No manual shift/or fallbacks needed on ARMv6+

#### D.4 Carry Flag Inversion — Correct but Expensive

**`codegen_arm.cpp:526-539`**:
```arm
CMP   Rd, Rs
MRS   REG_WORK1, CPSR        ; 1 cycle + pipeline stall
EOR   REG_WORK1, ARM_C_FLAG  ; Invert carry
MSR   CPSR, REG_WORK1        ; 1 cycle + pipeline stall
```

ARM sets Carry on "no borrow"; 68k sets Carry on "borrow". The EOR inverts to match. **Correct** but costs 3 extra instructions + 2 pipeline stalls per CMP/SUB operation on Cortex-A53.

#### D.5 ICache Invalidation — Correct

Uses Linux `cacheflush` syscall (`__NR_ARM_cacheflush = 0xf0002`) at:
- After `create_popalls()` — popallspace initialization
- After each `compile_block()` — per-block flush
- After `write_jmp_target()` — branch target patching

Called at correct granularity (per-block, not whole-cache). The `__clear_cache` GCC builtin is declared but unused; the SWI approach works on ARM32 Linux.

#### D.6 DMB Barriers in Popall Exit Stubs

**`compemu_support.cpp:4120-4163`**: Every popall exit stub emits `DMB ISH` (inner-shareable data memory barrier) before restoring registers and returning. This ensures JIT stores are visible to other cores on block exit. **Correct.**

However, **no DMB is emitted within compiled blocks** — see original Finding 3 (video concurrency section) for frame buffer implications.

#### D.7 BLX_i Encoding Bug (Dead Code)

**`codegen_arm.h:958`**: `BLX_i(i)` encodes as `0xEA000000|i` which is `B` (branch), not BLX. The correct BLX immediate encoding has `cond=0xF`. However, `BLX_i` is **never used** — only `BLX_r` (register form, correctly encoded) is used for function calls.

#### D.8 LDRH Offset Range

**`codegen_arm.cpp:997-1026`**: `raw_mov_w_rR` asserts `isbyte(offset)` allowing range -128..127, but LDRH supports 0..255 unsigned offset. Overly conservative — offsets 128..255 would hit `abort()` when `COMP_DEBUG=1`.

#### D.9 FPU — Entirely Unimplemented

**`codegen_arm.cpp:2024-2052`**: All FPU codegen functions (fmov, fint, fsqrt, fabs, etc.) call `jit_unimplemented()` which aborts. FPU opcodes fall back to the interpreter unconditionally.

#### D.10 No Alignment Fault Emulation

JIT-generated LDRH/LDR from guest memory relies on ARMv6+ unaligned access support. If a 68k program violates alignment (bus error on real hardware), the JIT silently performs an unaligned access. On Cortex-A53 with SCTLR.A=0 (default Linux), this works but is slow and hides a 68k bus error.

---

### E. Memory Access in JIT — The Critical Path

#### E.1 The Full Read Chain (canbang=1)

Tracing `readlong()` for MOVEA.L d16(An), An:

```
readlong(address, dest, tmp)                    [compemu_support.cpp:3598]
  → readmem_real(address, dest, 4, tmp)         [compemu_support.cpp:3527]
    if (canbang) {                              // ALWAYS TRUE
      → mov_l_brR(dest, address, MEMBaseDiff)   [compemu_midfunc_arm.cpp:880]
        → raw_mov_l_brR(d, s, offset)           [codegen_arm.cpp:745]
          LDR REG_WORK1, [PC, #offs]            // Load MEMBaseDiff
          LDR d, [REG_WORK1, s]                 // host_addr = MEMBaseDiff + guest_addr
                                                // *** NO BOUNDS CHECK ***
      → mid_bswap_32(dest)                      // REV instruction (byte-swap)
    }
```

**BasiliskII vs UAE path**: In the UAE path (not compiled for BasiliskII), `readlong()` has an additional `distrust_long()` / `special_mem` check that can redirect to the safe C++ accessor. The BasiliskII path goes **directly to `readmem_real()`** every time:

```cpp
void readlong(int address, int dest, int tmp)
{
    // NOTE: No #ifdef UAE distrust check here for BasiliskII
    readmem_real(address, dest, 4, tmp);
}
```

#### E.2 The Full Write Chain (canbang=1)

```
writelong(address, source, tmp)
  → writemem_real(address, source, 4, tmp)
    if (canbang) {
      mov_l_rr(f, source)
      mid_bswap_32(f)                           // REV (byte-swap)
      → mov_l_bRr(address, f, MEMBaseDiff)      // STR [MEMBaseDiff + guest_addr]
                                                 // *** NO BOUNDS CHECK ***
    }
```

#### E.3 `get_n_addr()` — Address Conversion Without Validation

**`compemu_support.cpp:3631-3635`**:
```cpp
if (canbang) {
    lea_l_brr(dest, address, MEMBaseDiff);      // ADD dest, addr, MEMBaseDiff
}
```
Used for MOVEM and string operations. Returns a host pointer from a guest address with zero validation.

#### E.4 Constant Folding in Register Allocator

When the register allocator knows a value at compile time (`isconst()`):
```cpp
// In mov_l_brR:
if (isconst(s)) {
    COMPCALL(mov_l_rm)(d, live.state[s].val + offset);
    return;
}
```
This computes `constant_address + offset` at JIT compile time, then generates a load from that fixed host address. **No bounds checking here either.**

---

### F. Block Cache Management

#### F.1 Compilation Thresholds

**`compemu_support.cpp:368`**:
```cpp
static int optcount[10] = { 10, 0, 0, 0, 0, 0, -1, -1, -1, -1 };
```

A block runs 10 times at optlev 0 (interpreted via `exec_nostats`), then jumps to optlev 2 (full compilation) since `optcount[1]=0`. Reasonable threshold.

#### F.2 Block Structure

Each compiled block has:
- `direct_handler` — Entry, jumps directly to compiled code
- `handler` — Non-direct entry that verifies `regs.pc_p` matches
- `direct_pen` / `direct_pcc` — Pending entry / checksum-verify entry (soft flush)
- `c1` / `c2` — Checksum pair for self-modifying code detection
- `dep[2]` — Forward jump dependencies
- `deplist` — Reverse dependency list

#### F.3 Self-Modifying Code Detection — Weak Checksums

**`compemu_support.cpp:3825-3865`**:
```cpp
k1 += *pos;    // Additive
k2 ^= *pos;    // XOR
```
Using additive + XOR checksums is collision-prone: a swap of two 32-bit words at the same positions produces identical k1 and k2. Acceptable in practice but theoretically bypassed by targeted modifications.

`LONGEST_68K_INST = 256` on ARM (vs 16 on x86) inflates checksum regions by 16×. With `MAX_CHECKSUM_LEN = 2048`, blocks longer than ~1792 bytes are flushed unconditionally. The actual longest 68k instruction is ~22 bytes, so 256 is excessively conservative.

#### F.4 Soft Flush vs Hard Flush

- **Hard flush**: Frees all blocks, resets compile buffer. Called on cache overflow or explicit `flush_icache()`.
- **Soft (lazy) flush**: Moves blocks to dormant, marks `BI_NEED_CHECK`. On next execution, checksum verification either reactivates or recompiles. More efficient for transient SMC patterns.

#### F.5 Block Linking

Conditional branches emit two paths (taken/not-taken), each with `create_jmpdep()` and specular inline cache hints. Each chained jump checks `spcflags == 0` before following the chain, ensuring interrupt responsiveness. **Correct.**

#### F.6 No Locking on Block Lists

`active`, `dormant`, `cache_tags[]` manipulated without synchronization. Safe in current single-threaded CPU emulation, fragile if concurrency is ever added.

---

### G. Exception Handling

#### G.1 TRY/CATCH/THROW — C++ Exceptions on ARM32

`EXCEPTIONS_VIA_LONGJMP` is **never defined**. The active definitions in `memory.h`:
```cpp
struct m68k_exception { int prb; ... };
#define TRY(var) try
#define CATCH(var) catch(m68k_exception var)
#define THROW(n) throw m68k_exception(n)
```

C++ exception unwinding requires proper `.ARM.unwind` / `.eh_frame` metadata. When `THROW(2)` fires from within JIT-boundary code, the C++ runtime must unwind through:
1. The memory accessor (`get_long`, etc.) — has unwind info ✅
2. The interpreter opcode handler (`cputbl[opcode]`) — has unwind info ✅
3. The JIT dispatch stub — **native ARM, no unwind info** ⚠️
4. Back to `m68k_compile_execute`'s TRY/CATCH

**This works in practice** because `compemu_raw_call()` uses `BL` (proper ARM call) from within the JIT stubs, and the C frames below have valid unwind info. The GCC ARM32 unwinder treats BL-created frames benignly. But it is **not guaranteed** by the C++ standard and depends on implementation-specific unwinder behavior.

#### G.2 Dual Exception Mechanism (Signal + C++ Exception)

**Level 1 — Signal handler** (`compemu_support.cpp:416-441`):
Catches SIGSEGV/SIGBUS from truly unmapped JIT memory accesses. Uses `siglongjmp` back to `m68k_do_compile_execute`.

**Level 2 — `m68k_do_compile_execute`** (`compemu_support.cpp:5543-5558`):
After `siglongjmp` recovery, sets `SPCFLAG_JIT_EXEC_RETURN` and calls `THROW(2)` to convert the signal into a C++ exception.

**Level 3 — `m68k_compile_execute` TRY/CATCH** (`compemu_support.cpp:5571-5612`):
Catches `m68k_exception(2)`. Response: **permanently disables JIT** and falls back to the pure interpreter.

```cpp
CATCH(prb) {
    if (exception_no == 2) {
        UseJIT = false;                    // PERMANENT DISABLE
        NotifyJITDisabledFallback();
        flush_icache();
        m68k_execute();                    // Never returns to JIT
        return;
    }
}
```

**Critical implication**: Any bus error from JIT execution is treated as fatal to the JIT. On a Quadra 800 ROM that probes memory ranges, the JIT will be permanently disabled very early during boot if ROM code touches any unmapped address. This explains the observed behavior where `THROW(2)` from `get_long()` kills the JIT immediately.

The **correct behavior** would be to deliver the bus error to the 68k exception system via `Exception(2, 0)` and resume JIT execution. The current "nuclear" approach was likely a debugging expedient.

#### G.3 PC Stale on JIT Exception

**Signal handler at `compemu_support.cpp:432-433`**:
```cpp
regs.fault_pc = regs.pc;    // Uses regs.pc directly
```

In the JIT path, `regs.pc` is **not updated per-instruction** — only `regs.pc_p` is maintained. The signal handler stores a potentially stale guest PC. Should use `m68k_getpc()` or ensure `regs.pc` is kept current in JIT blocks. However, since the current response is "disable JIT entirely," exact fault PC matters less.

#### G.4 No Per-Callout Exception Guard

Within a JIT block, interpreter fallback calls at `compemu_support.cpp:5183-5192`:
```cpp
compemu_raw_mov_l_mi((uintptr)&regs.pc_p, (uintptr)pc_hist[i].location);
compemu_raw_call((uintptr)cputbl[opcode]);
```

There is **no TRY/CATCH around individual interpreter call-outs**. An exception propagates all the way up to `m68k_compile_execute`'s catch handler, triggering the permanent JIT disable. A per-callout TRY/CATCH could catch the exception and deliver it as a proper 68k bus error without destroying the JIT.

#### G.5 Double Bus Error Protection

`Exception()` in `newcpu.cpp:558-628` uses a nested try/catch when building the 68040 bus error stack frame. If `put_long()` throws during stack frame construction (e.g., SP points to invalid memory), the double fault is caught:
```cpp
try { ... build frame ... }
catch (m68k_exception) { report_double_bus_error(); }
```
**This is correct.**

---

### H. Constants & Data Structures

| Constant | Value | Assessment |
|----------|-------|------------|
| `TAGMASK` | `0x0000FFFF` | Cache hash mask, 64K entries |
| `TAGSIZE` | 65536 | Cache tag array size |
| `MAXRUN` | 1024 | Max instructions per block |
| `N_REGS` | 13 (ARM) | r0-r12 available for allocation |
| `BYTES_PER_INST` | 10240 | Margin per instruction (conservative) |
| `LONGEST_68K_INST` | 256 (ARM) / 16 (x86) | Drives checksum length — 16× inflated |
| `MAX_CHECKSUM_LEN` | 2048 | Max checksum bytes |
| `POPALLSPACE_SIZE` | 2048 | May be tight with `DATA_BUFFER_SIZE=1024` |
| `STACK_ALIGN` | 4 (ARM) | Correct for ARM32 EABI |

The `uae_p32(x)` macro on ARM32 is just `(uae_u32)(x)` — no 64-bit truncation check needed.

---

### I. Consolidated Issue List

#### Critical

| # | Issue | Location | Impact |
|---|-------|----------|--------|
| C1 | **JIT memory reads have ZERO bounds checking** (canbang=1) | `readmem_real()` → `mov_l_brR()` → ARM `LDR [MEMBaseDiff + EA]` | Root cause: out-of-range guest EA loads garbage from arbitrary host memory |
| C2 | **JIT memory writes have ZERO bounds checking** (canbang=1) | `writemem_real()` → `mov_l_bRr()` → ARM `STR [MEMBaseDiff + EA]` | Can corrupt arbitrary host memory if guest EA is out of range |
| C3 | **Bus error permanently kills JIT** | `m68k_compile_execute()` CATCH for exception 2 | Any single bus error (even expected ROM probing) disables JIT for session |
| C4 | **`is_direct_address_valid()` has wrong frame buffer address** for DIRECT_ADDRESSING | `memory.h:65-82`, hardcoded `0xa0000000` | Interpreter path may throw on valid framebuffer accesses |

#### High

| # | Issue | Location | Impact |
|---|-------|----------|--------|
| H1 | **No `distrust_long()` / `special_mem` in BasiliskII path** | `readlong()` at `compemu_support.cpp:3598` | All reads go directly to `readmem_real()` — no fallback to safe accessors |
| H2 | **`get_n_addr()` returns unvalidated host pointer** | `compemu_support.cpp:3631-3635` | MOVEM etc. get raw host pointer from unvalidated guest address |
| H3 | **IMM32 macro silently returns 0** on non-representable immediates | `codegen_arm.h:70-87` | Future code passing non-rotatable constants silently encodes `#0` |
| H4 | **No per-callout TRY/CATCH** for interpreter fallback in JIT blocks | `compemu_support.cpp:5183-5192` | Single exception kills entire JIT rather than delivering 68k bus error |
| H5 | **C++ exception unwind through JIT stubs** relies on implementation-specific ARM unwinder behavior | Architecture-wide | Works in practice but not guaranteed by C++ standard |

#### Medium

| # | Issue | Location | Impact |
|---|-------|----------|--------|
| M1 | **Carry inversion costs 3 extra insns + 2 stalls** per CMP/SUB | `codegen_arm.cpp:526-539` | Performance: MRS/MSR pipeline stalls on Cortex-A53 |
| M2 | **Only 9 ARM registers** for 16 m68k regs | `codegen_arm.cpp:53-100` | Heavy register spilling, fundamental ARM limitation |
| M3 | **LDRH offset range too conservative** (`isbyte` check) | `codegen_arm.cpp:997-1026` | Values 128-255 hit abort() with `COMP_DEBUG=1` |
| M4 | **No alignment fault emulation** | Architecture-wide | Unaligned 68k accesses silently work on ARMv6+ instead of bus erroring |
| M5 | **Weak checksums** for SMC detection | `compemu_support.cpp:3825-3865` | Additive+XOR collision possible (unlikely in practice) |
| M6 | **`LONGEST_68K_INST=256`** inflates checksum regions 16× | `compemu.h` | Unnecessarily flushed blocks, wasted checksumming |
| M7 | **`regs.fault_pc` stale** in JIT signal handler | `compemu_support.cpp:432-433` | Incorrect PC reported on JIT fault (mitigated by JIT disable) |
| M8 | **FPU entirely unimplemented in ARM codegen** | `codegen_arm.cpp:2024-2052` | All FPU falls back to interpreter (performance only) |

#### Low / Informational

| # | Issue | Location | Notes |
|---|-------|----------|-------|
| L1 | `compemu_midfunc_arm2.cpp` is dead code (`USE_JIT2` undefined) | `compemu_support.cpp:2739` | 2200+ lines never compiled |
| L2 | BLX_i encoding is wrong (encodes B not BLX) | `codegen_arm.h:958` | Dead code — never called |
| L3 | Dead `tmps` scratch register in i_MOVEA | `gencomp.c:1700` | Minor waste per MOVEA compilation |
| L4 | `__clear_cache` declared but unused | `codegen_arm.cpp:46` | SWI-based flush used instead (both work) |
| L5 | `POPALLSPACE_SIZE=2048` may be tight | `compemu.h` | Comment says enlarge if `DATA_BUFFER_SIZE > 768`; it's 1024 |
| L6 | No locking on block lists | `compemu_support.cpp:700-815` | Safe single-threaded, fragile if concurrency added |

---

### J. Root Cause Chain — Complete Reconstruction

```
  Boot: 68k ROM code (Quadra 800) initializes memory manager
    │
    ▼
  An (address register) receives a pointer near RAM boundary
    │
    ▼
  MOVEA.L d16(An), An  [opcode 0x2068]
  EA = An + sign_extend_16(displacement)
  EA = e.g. 0x088D36EC  ← just past RAMSize (0x08800000)
    │
    ▼
  JIT path (canbang=1):
    readmem_real() → mov_l_brR(dest, EA, MEMBaseDiff)
    → ARM: LDR Rd, [RAMBaseHost + 0x088D36EC]
    → Loads from host memory 0x088D36EC past RAM allocation
    → Host memory happens to be mapped (stack/libs/heap)
    → Reads garbage: 0x0040F150
    → After REV byte-swap: 0x50F14000
    │
    ▼
  Value 0x50F14000 stored into A3 (regs.regs[11])
    │
    ▼
  Later: op_4a28_0_ff uses A3 (0x50F14000) as EA
    → Interpreter fallback: get_long(0x50F14000)
    → is_direct_address_valid(0x50F14000) → false
    → THROW(2)
    │
    ▼
  m68k_compile_execute CATCH: exception_no == 2
    → UseJIT = false   ← PERMANENT JIT DISABLE
    → Falls back to pure interpreter
```

**Why the interpreter would handle this correctly**: The interpreter's `get_long(EA)` for the MOVEA.L would detect `EA = 0x088D36EC` is outside RAM and THROW(2), generating a proper 68k bus error. The 68k OS bus error handler would see this, handle it (possibly by adjusting the address or reporting an error), and execution would continue. The JIT skips this check entirely, so the garbage value propagates silently.

---

### K. Recommended Fixes (Priority Order)

> **Implementation Status (2026-02-22):** K.1, K.2, K.3, and K.5 have been implemented.
> See commit details below each fix.

#### K.1 — Add bounds checking to JIT memory access (fixes C1, C2) — **IMPLEMENTED**

Instead of inline compare-and-branch (which requires complex raw ARM emission inside the register allocator), the implementation routes all JIT `readbyte`/`readword`/`readlong`/`writebyte`/`writeword`/`writelong` through C helper functions (`jit_read_long`, `jit_write_long`, etc.) that call the bounds-checked `get_long`/`put_long` accessors. These helpers THROW(2) on out-of-range addresses.

This required unlocking `call_r_11`, `call_r_02`, and `compemu_raw_call_r` from `#if defined(UAE)` guards — these are generic register-allocator-aware function call primitives with nothing UAE-specific.

**Files modified:**
- `compemu_support.cpp`: Added 6 helper functions, modified 6 read/write functions
- `compemu_midfunc_arm.cpp`: Removed UAE guards from `call_r_02` and `call_r_11`
- `codegen_arm.cpp`: Removed UAE guard from `compemu_raw_call_r`

**Trade-off:** Every JIT memory access is now a function call. This is slower than inline direct access but ensures correctness. `get_n_addr()` (used by MOVEM) retains the direct path — faults there are caught by the SIGSEGV signal handler.

**Future optimization:** Add inline CMP+BHS guard for RAM-range fast path, falling through to the C helper only for out-of-range addresses.

#### K.2 — Make bus error recoverable (fixes C3, H4) — **IMPLEMENTED**

Changed `m68k_compile_execute`'s exception-2 handler from permanent JIT disable to recoverable bus error delivery with a budget. The first 50 bus errors are delivered via `Exception(2, 0)` with JIT cache flush and `goto setjmpagain`. After 50 cumulative bus errors, the JIT is disabled as a safety measure.

**File modified:** `compemu_support.cpp` — `m68k_compile_execute()` CATCH block

#### K.3 — Fix `is_direct_address_valid()` frame buffer address (fixes C4) — **IMPLEMENTED**

Replaced hardcoded `0xa0000000` with dynamic computation from `MacFrameBaseHost` (the actual host pointer to the frame buffer). The Mac-side address is computed as `(uintptr)MacFrameBaseHost - MEMBaseDiff`. Also ensured that `MacFrameBaseHost` and `MacFrameSize` are set in the DIRECT_ADDRESSING path of `set_mac_frame_buffer()`.

**Files modified:**
- `memory.h`: Added `MacFrameBaseHost` extern, dynamic frame base computation
- `video_sdl2.cpp`: Set `MacFrameBaseHost`/`MacFrameSize` in DIRECT_ADDRESSING path

#### K.4 — Port `distrust_*`/`special_mem` to BasiliskII path (fixes H1)

Enable the UAE-style `distrust_long()` check in the BasiliskII path as a fallback for I/O regions and frame buffer addresses, allowing those accesses to go through the safe C++ accessors while keeping the fast direct path for RAM.

#### K.5 — Add IMM32 assertion (fixes H3) — **IMPLEMENTED**

In debug builds (`JIT_DEBUG`), IMM32 now calls a checked function that aborts with a diagnostic message when a non-representable constant is encountered. Release builds retain the original macro (silent zero fallback) for performance.

**File modified:** `codegen_arm.h` — conditional `IMM32` macro with `_imm32_checked()` for debug

---

## Executive Summary (Original Video Corruption Analysis)

The patterned screen corruption is caused by **unsynchronized concurrent access** to the Mac frame buffer (`the_buffer`) between the JIT CPU emulation thread and the SDL redraw thread. The JIT compiler emits raw ARM `STR` instructions that write directly to host memory with no locking, no memory barriers, and no dirty-region notification. The display refresh thread simultaneously reads the same memory via `memcmp`/`memcpy` on a separate CPU core. This produces torn reads, missed updates, and tile-aligned visual artifacts.

Secondary issues compound the problem: missing ARM memory barriers (`DMB`), the VOSF (mprotect-based dirty tracking) path being fundamentally incompatible with JIT direct writes, and the 64×64 pixel tile scanning granularity creating visible grid-aligned corruption patterns.

---

## 1. Video Pipeline Architecture

### Threading Model

```
┌─────────────────────────────────────────────────────────┐
│ Main Thread (CPU Emulation)                             │
│                                                         │
│   m68k instruction                                      │
│       │                                                 │
│       ▼                                                 │
│   JIT compiled block (ARM native code)                  │
│       │                                                 │
│       ▼                                                 │
│   writemem_real() → STR to the_buffer + MEMBaseDiff     │
│       (no lock, no barrier, no dirty notification)      │
│                                                         │
│   VideoInterrupt() called periodically:                 │
│       → SDL_PumpEvents()                                │
│       → present_sdl_video()                             │
│           → SDL_BlitSurface (guest → host surface)      │
│           → SDL_UpdateTexture (host surface → GPU)      │
│           → SDL_RenderPresent                           │
│       → UNLOCK_FRAME_BUFFER / LOCK_FRAME_BUFFER         │
└─────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────┐
│ Redraw Thread (SDL "Redraw Thread")                     │
│                                                         │
│   redraw_func() loop @ 60 Hz:                           │
│       → handle_events()                                 │
│       → video_refresh()                                 │
│           → update_display_static_bbox() [non-VOSF]     │
│               → memcmp(the_buffer, the_buffer_copy)     │
│               → memcpy(the_buffer_copy ← the_buffer)   │
│               → Screen_blit(drv->s->pixels ← the_buf)  │
│               → update_sdl_video() [queue dirty rect]   │
│           OR                                            │
│           → update_display_window_vosf() [VOSF]         │
│               → find dirty pages via dirtyPages[]       │
│               → vm_protect(VM_PAGE_READ) [re-protect]   │
│               → Screen_blit(host ← the_buffer)         │
│       → handle_palette_changes()                        │
└─────────────────────────────────────────────────────────┘
```

### Key Data Structures

| Buffer | Purpose | Written by | Read by |
|--------|---------|-----------|---------|
| `the_buffer` | Mac frame buffer (host memory) | JIT CPU thread (raw STR) | Redraw thread (memcmp/Screen_blit) |
| `the_buffer_copy` | Shadow copy for change detection | Redraw thread (memcpy) | Redraw thread (memcmp) |
| `drv->s->pixels` (guest_surface) | SDL surface pixels | Redraw thread (Screen_blit) | present_sdl_video (SDL_BlitSurface) |
| `host_surface->pixels` | Converted pixels for texture | present_sdl_video (SDL_BlitSurface) | present_sdl_video (SDL_UpdateTexture) |

---

## 2. Finding: Unsynchronized Concurrent Access (PRIMARY ROOT CAUSE)

### Severity: **CRITICAL**

### Location

- JIT writes: `BasiliskII/src/uae_cpu_2021/compiler/compemu_support.cpp` lines 3339–3357
- Redraw reads: `BasiliskII/src/SDL/video_sdl2.cpp` lines 2873–2970

### Mechanism

The JIT compiler's `writemem_real()` function generates direct native ARM stores:

```cpp
// compemu_support.cpp line 196 (BasiliskII path)
#define canbang 1

// compemu_support.cpp line 3339
static void writemem_real(int address, int source, int size, int tmp, int clobber)
{
    int f=tmp;
    if (canbang) {  // Always true for BasiliskII
        switch(size) {
            case 1: mov_b_bRr(address,source,MEMBaseDiff); break;  // STRB
            case 2: mov_w_rr(f,source); mid_bswap_16(f);
                    mov_w_bRr(address,f,MEMBaseDiff); break;        // REV16 + STRH
            case 4: mov_l_rr(f,source); mid_bswap_32(f);
                    mov_l_bRr(address,f,MEMBaseDiff); break;        // REV + STR
        }
        return;
    }
    // ... bank-dispatched path (never reached)
}
```

These compile to ARM instructions like:

```asm
REV     r6, r6          ; byte-swap for big-endian Mac
STR     r6, [r7, r2]    ; direct store to the_buffer via MEMBaseDiff
```

The `special_mem` dispatching (which could intercept frame buffer writes) exists only behind `#ifdef UAE`, which is **not defined** for BasiliskII:

```cpp
void writebyte(int address, int source, int tmp)
{
#ifdef UAE                           // ← NOT defined for BasiliskII
    if ((special_mem & S_WRITE) || distrust_byte())
        writemem_special(...);
    else
#endif
        writemem_real(...);          // ← Always takes this path
}
```

Meanwhile, the display refresh runs on a separate SDL thread with no synchronization:

```cpp
// video_sdl2.cpp, update_display_static_bbox() — redraw thread
for (uint32 y = 0; y < VIDEO_MODE_Y; y += N_PIXELS) {   // N_PIXELS = 64
    for (uint32 x = 0; x < VIDEO_MODE_X; x += N_PIXELS) {
        for (uint32 j = y; j < (y + h); j++) {
            // JIT can write to the_buffer HERE, between memcmp and memcpy
            if (memcmp(&the_buffer[yb + xb], &the_buffer_copy[yb + xb], xs) != 0) {
                memcpy(&the_buffer_copy[yb + xb], &the_buffer[yb + xb], xs);
                Screen_blit((uint8 *)drv->s->pixels + dst_yb + xb, the_buffer + yb + xb, xs);
                dirty = true;
            }
        }
    }
}
```

### Race Scenarios

**Scenario A — Missed update (most common, produces "stuck" tiles)**:
1. JIT writes new data to tile (x=128, y=64)
2. Redraw thread's `memcmp` reaches that tile, detects change
3. JIT writes **more** data to the same tile
4. Redraw thread does `memcpy(the_buffer_copy ← the_buffer)` — copies the newest data
5. Redraw thread does `Screen_blit(display ← the_buffer)` — blits the newest data
6. **BUT**: the data written in step 3 is now in `the_buffer_copy` without having been detected as "new" for the *next* frame
7. Next frame: `memcmp` says "no change" (both buffers match) → tile is stale on screen until another write

**Scenario B — Torn scanline (produces horizontal stripe artifacts)**:
1. Redraw thread starts `memcmp` on scanline j
2. JIT writes to the middle of scanline j
3. `memcmp` returns "different" (detected the partial write)
4. `Screen_blit` copies the line — but JIT continues writing during the blit
5. Left half of line has old data, right half has new → visible tear

**Scenario C — Invisible write (produces tile-grid corruption)**:
1. Redraw thread finishes checking tile (64, 128) — no changes detected
2. JIT immediately writes to that tile
3. Next `memcpy(the_buffer_copy)` for that tile won't happen until the tile is "dirty" again
4. The write is invisible until another write to the same 64×64 tile triggers detection

### Why the Pattern is Grid-Aligned

The 64-pixel tile scanning in `update_display_static_bbox()` creates a **visible grid pattern** because corruption boundaries align with tile edges. A write that spans two tiles may be detected in one tile but missed in the adjacent one.

### `LOCK_FRAME_BUFFER` Does Not Help

The `frame_buffer_lock` mutex exists but is only acquired during:
- Video mode initialization (`driver_base::init()`)
- `VideoInterrupt()` / `VideoVBL()` — briefly released and re-acquired as a "scheduling point"

It is **never locked** during `update_display_static_bbox()` or `update_display_static()`. The redraw thread runs unlocked.

---

## 3. Finding: No ARM Memory Barriers in JIT Codegen

### Severity: **HIGH**

### Location

- `BasiliskII/src/uae_cpu_2021/compiler/codegen_arm.cpp` (entire file — zero `DMB`/`DSB`/`ISB` instructions)
- `BasiliskII/src/uae_cpu_2021/compiler/compemu_midfunc_arm.cpp`

### Mechanism

On ARMv7 SMP (Raspberry Pi 3's four Cortex-A53 cores), stores from one core are not guaranteed to be visible to another core without a Data Memory Barrier. The JIT emits plain `STR` instructions without any barrier:

```
; JIT-generated code for a m68k MOVE.L to video memory
REV     r6, r6              ; byte-swap
STR     r6, [r7, r2]        ; store to the_buffer — may sit in core 0's write buffer
; (no DMB here)
```

The redraw thread on core 1 may read stale L1 cache lines for `the_buffer`, seeing old data even though the JIT has already written new data. This produces **non-deterministic** corruption that varies between runs and is sensitive to CPU load.

The only cache operation in the JIT is `flush_cpu_icache()` (via `sys_cacheflush` syscall) in `compemu_midfunc_arm.cpp` line 1975, which flushes the **instruction cache** for newly-compiled JIT blocks — it does not affect data visibility.

### Impact

Without `DMB`, the redraw thread's `memcmp` may:
- See partially-updated cache lines (e.g., first 4 bytes of a 32-byte cache line updated, rest stale)
- Read an old version of a store that the JIT performed thousands of cycles ago
- Produce corruption that appears and disappears unpredictably

---

## 4. Finding: VOSF Is Fundamentally Incompatible with JIT Direct Writes

### Severity: **HIGH** (for JIT+VOSF build variant)

### Location

- `BasiliskII/src/CrossPlatform/video_vosf.h` lines 432–448 (Screen_fault_handler)
- `BasiliskII/src/CrossPlatform/video_vosf.h` lines 511–547 (update_display_window_vosf)
- `BasiliskII/src/Unix/main_unix.cpp` lines 270–290 (sigsegv_handler)

### Mechanism

VOSF works by:
1. Write-protecting the frame buffer pages with `mprotect(PROT_READ)`
2. When Mac code writes to video memory → SIGSEGV → `Screen_fault_handler`
3. Handler marks the page dirty (`PFLAG_SET`) and makes it writable (`mprotect(PROT_READ|PROT_WRITE)`)
4. The display update finds dirty pages, blits them, re-protects them

With JIT, the flow is:
1. VOSF re-protects a page (step 4 above)
2. JIT emits `STR` to that page → **SIGSEGV**
3. Signal handler runs, takes `LOCK_VOSF`, marks page dirty, makes writable
4. JIT's `STR` retries and succeeds

This produces **three problems**:

**Problem A — Extreme overhead**: Every JIT write to a re-protected video page triggers a full SIGSEGV signal delivery, `mprotect` syscall in the handler, and instruction restart. For a 640×480×8bpp frame buffer (~300 KB, ~75 pages), a full-screen update triggers 75+ SIGSEGV signals per frame. At 60 fps, that's 4,500 signal handler invocations per second.

**Problem B — Race window in update_display_window_vosf**:

```cpp
// video_vosf.h, update_display_window_vosf()
PFLAG_CLEAR_RANGE(first_page, page);        // ① Mark pages "clean"

// Make the dirty pages read-only again
vm_protect(..., VM_PAGE_READ);               // ② Re-protect

// JIT can SIGSEGV here, mark page dirty again, make it writable
// But PFLAG was already cleared in ①
// The blit below won't know about the JIT's latest write

Screen_blit(the_host_buffer + i2,            // ③ Blit (may see or miss JIT's write)
            the_buffer + i1, src_bytes_per_row);
```

Between ① and ③, a JIT write re-dirties the page but the blit may or may not see the new data. The dirty flag is set, but it was cleared in ① — the current iteration won't process it again, and whether the *next* iteration catches it depends on timing.

**Problem C — Mutex in signal handler**: `Screen_fault_handler` acquires `LOCK_VOSF` (a `pthread_mutex_t`). POSIX allows `pthread_mutex_lock` in signal handlers only if the mutex is not held by the interrupted thread. If the CPU emulation thread is also the thread that calls `update_display_window_vosf` (which holds `LOCK_VOSF`), and a SIGSEGV fires on that same thread, this is **undefined behavior** and will typically deadlock. In practice, with `USE_PTHREADS_SERVICES` (the BasiliskII default on Linux), the display update runs on a separate thread, so this specific deadlock is unlikely — but it remains a correctness hazard.

### Current CI Matrix Interaction

| Build Variant | JIT | VOSF | Corruption Source |
|---------------|-----|------|-------------------|
| `basilisk2-arm32-jit` | ✅ | ❌ | Data race (Finding 2) + no DMB (Finding 3) |
| `basilisk2-arm32-jit-vosf` | ✅ | ✅ | All of the above + VOSF race (Finding 4) |
| `basilisk2-arm32-nojit` | ❌ | ❌ | Should be clean (interpreted CPU + polling) |
| `basilisk2-arm32-nojit-vosf` | ❌ | ✅ | Should be clean (writes trigger SIGSEGV correctly) |

---

## 5. Finding: Blitter Operates on Unguarded 64-bit Loads/Stores

### Severity: **MEDIUM**

### Location

- `BasiliskII/src/CrossPlatform/video_blit.h` lines 100–170

### Mechanism

With `UNALIGNED_PROFITABLE` defined (set for ARM in `configure.ac` line 1712), the blitter skips alignment preambles and goes straight to the Duff's device loop with 64-bit (`uint64`) loads and stores:

```cpp
// video_blit.h — the inner blit loop
// With UNALIGNED_PROFITABLE, alignment preamble is skipped
if (length >= 8) {
    // Duff's device with 64-bit operations
    FB_BLIT_4(DEREF_QUAD_PTR(dest, -8), DEREF_QUAD_PTR(source, -8));
    // ...
}
```

On ARM, `LDRD`/`STRD` (64-bit load/store) are **not atomic** — they decompose into two 32-bit operations. If the JIT writes a 32-bit word to `the_buffer` while the blitter is doing a 64-bit read of the same location, the blitter can read half-old/half-new data. This is a secondary amplification of Finding 2.

---

## 6. Finding: `update_display_static_bbox` 16-bit Mode Path Uses `Screen_blit` Without Checking All Tiles

### Severity: **LOW** (cosmetic, already partially fixed)

### Location

- `BasiliskII/src/SDL/video_sdl2.cpp` lines 2873–2970

### Mechanism

The `update_display_static_bbox()` function applies `Screen_blit` only when `blit` is true, which is set only for `VIDEO_DEPTH_16BIT`:

```cpp
bool blit = (int)VIDEO_MODE_DEPTH == VIDEO_DEPTH_16BIT;
```

For 8-bit paletted mode, `blit` is false, so the function does `memcmp` + `memcpy` to `the_buffer_copy` but **does not** blit to `drv->s->pixels`. The actual pixel conversion (palette lookup, expansion) happens later in `present_sdl_video()` via `SDL_BlitSurface(guest_surface → host_surface)`. This indirect path is correct but means the `guest_surface->pixels` must point to `the_buffer` (for non-VOSF) — which it does. However, the unsynchronized reads from `the_buffer` during `SDL_BlitSurface` repeat the race condition from Finding 2.

For 32-bit mode, `host_surface == guest_surface`, and `guest_surface` was created from `the_buffer` via `SDL_CreateRGBSurfaceFrom`. This means `SDL_BlitSurface` is a no-op (source == dest), and `SDL_UpdateTexture` reads directly from `the_buffer` — **again unsynchronized** with JIT writes.

---

## 7. Comparison: Why Non-JIT Builds Are (Mostly) Correct

In interpreted mode (`--disable-jit-compiler`), the CPU emulation runs through `m68k_execute()` which calls C functions like `WriteMacInt32()` for every memory access. These functions are on the same thread as the caller and have natural synchronization points — the emulator checks for interrupts (including `VideoInterrupt`) at regular intervals, creating implicit "yield points" where the frame buffer is in a consistent state.

With JIT, the compiled native code runs for potentially thousands of host instructions without any yield point, and the stores go directly to host memory bypassing any per-access checking.

---

## Appendix A: Relevant Source Files

| File | Role |
|------|------|
| `BasiliskII/src/uae_cpu_2021/compiler/compemu_support.cpp` | JIT compiler core — `writemem_real()`, `canbang`, block compilation |
| `BasiliskII/src/uae_cpu_2021/compiler/codegen_arm.cpp` | ARM code generation — `mov_*_bRr()` emit `STR`/`STRH`/`STRB` |
| `BasiliskII/src/uae_cpu_2021/compiler/compemu.h` | JIT data structures — `special_mem`, `blockinfo` |
| `BasiliskII/src/SDL/video_sdl2.cpp` | SDL2 video driver — display update, texture presentation |
| `BasiliskII/src/CrossPlatform/video_vosf.h` | VOSF — mprotect-based dirty page tracking, `Screen_fault_handler` |
| `BasiliskII/src/CrossPlatform/video_blit.cpp` | Pixel format blitters — `Screen_blit`, `Screen_blitter_init` |
| `BasiliskII/src/CrossPlatform/video_blit.h` | Blitter inner loop template — Duff's device with 64-bit ops |
| `BasiliskII/src/Unix/main_unix.cpp` | SIGSEGV handler dispatch, `sigsegv_handler()` |
| `BasiliskII/src/CrossPlatform/sigsegv.cpp` | SIGSEGV infrastructure |

## Appendix B: How to Reproduce

```bash
# Build the JIT variant
# (uses GitHub Actions — see .github/workflows/build-arm-jit.yml)

# On Raspberry Pi 3B, run with 8-bit depth for maximum visibility:
./BasiliskII-arm32-jit --screen win/640/480/8

# Corruption is most visible during:
# - Boot splash screen drawing
# - Finder desktop pattern rendering
# - Window drag/resize operations
# - Scrolling in any application

# Debug environment variables:
export B2_DEBUG_VIDEO=1    # Log video pipeline state
export B2_DEBUG_PIXELS=1   # Dump pixel values during blit
export B2_RAW_16BIT=1      # Bypass Screen_blit for 16-bit (diagnostic)
```

## Appendix C: Related Codebases

The ARM JIT codegen was ported from **ARAnyM** (Atari Running on Any Machine). ARAnyM's video path uses a different architecture — it has a NatFeats (Native Features) interface where the guest OS explicitly notifies the host of screen updates. This avoids the SIGSEGV/polling problem entirely. The BasiliskII VOSF mechanism predates JIT support and was designed for interpreted-only execution.

The x86 JIT in the older `uae_cpu/compiler/` directory has the same `canbang=1` direct-write behavior, but x86's stronger memory model (TSO — Total Store Order) and typically single-threaded display update (via `USE_CPU_EMUL_SERVICES` on x86 Linux) masks the race condition.
