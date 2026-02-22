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

## Executive Summary

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
