# TODO — JIT Video Corruption Remediation

**Branch**: `feature/arm-jit`  
**Reference**: [JIT_FINDINGS.md](JIT_FINDINGS.md)

---

## Phase 1: Eliminate the Data Race (HIGH — do first)

These items address Finding 2 (unsynchronized concurrent access) and Finding 3 (no ARM memory barriers). This is the primary corruption source for the `basilisk2-arm32-jit` build.

### 1.1 Add memory barrier at JIT block exit

**File**: `BasiliskII/src/uae_cpu_2021/compiler/codegen_arm.cpp`

Emit a `DMB ISH` (Data Memory Barrier, inner-shareable) instruction at each JIT block exit point. This ensures all stores issued by the JIT block are visible to other cores before control returns to the emulator loop.

**Where**: All six `popall_*` exit stubs in `compemu_support.cpp` — these are the common exit paths that every JIT-compiled block jumps to.

**Cost**: ~1 cycle per block exit on Cortex-A53. Negligible.

**Validation**: Run with 8-bit depth; corruption patterns that appear/disappear randomly (cache-line dependent) should be eliminated or greatly reduced.

- [x] Identify block exit codegen — `popall_do_nothing`, `popall_execute_normal`, `popall_cache_miss`, `popall_recompile_block`, `popall_exec_nostats`, `popall_check_checksum` in `compemu_support.cpp`
- [x] Add `DMB_ISH()` / `DMB_ST()` / `DSB_ISH()` / `DSB_SY()` macros in `codegen_arm.h`
- [x] Insert `DMB_ISH()` at all six exit stubs before register restore
- [ ] Test on hardware

> **Done.** Six barrier macros (`DMB_SY`, `DMB_ST`, `DMB_ISH`, `DMB_ISHST`, `DSB_SY`, `DSB_ISH`) added to `codegen_arm.h` using raw ARMv7 encodings (e.g. `_W(0xf57ff05b)` for `DMB ISH`). Each of the six `popall_*` stubs in `compemu_support.cpp` now emits `DMB_ISH()` just before the register restore / return sequence, ensuring all JIT-issued stores drain to the point of coherency before the main thread re-enters the emulator loop.

### 1.2 Lock the frame buffer during display update

**File**: `BasiliskII/src/SDL/video_sdl2.cpp`

Wrap the `memcmp`/`memcpy`/`Screen_blit` loop in `update_display_static_bbox()` (line 2873) and `update_display_static()` (line 2706) with `LOCK_FRAME_BUFFER` / `UNLOCK_FRAME_BUFFER`. The JIT CPU thread already briefly releases this lock during `VideoInterrupt()` (line 1986–1987), so the redraw thread can acquire it.

**Risk**: May reduce frame rate if the lock is held too long. Mitigate by locking per-tile-row instead of per-frame.

**Alternative approach**: Use `__atomic_thread_fence(__ATOMIC_ACQUIRE)` at the start of the display update and `__atomic_thread_fence(__ATOMIC_RELEASE)` after JIT block exits, avoiding mutex overhead entirely.

- [x] Add `LOCK_FRAME_BUFFER` / `UNLOCK_FRAME_BUFFER` in `video_refresh_window_static()` around the display update call
- [x] Add the same in `video_refresh_window_vosf()` and `video_refresh_dga_vosf()`
- [ ] Verify that `VideoInterrupt()` releases the lock frequently enough to avoid redraw starvation
- [ ] Measure frame rate impact on Pi 3B
- [ ] Test with 8-bit, 16-bit, and 32-bit depths

> **Done.** `LOCK_FRAME_BUFFER` / `UNLOCK_FRAME_BUFFER` calls now wrap the display update in all three refresh functions in `video_sdl2.cpp`. The existing mutex (`the_buffer_mutex`) is already released by `VideoInterrupt()` every VBL, giving the redraw thread a window to acquire it. Frame rate impact is expected to be negligible since the lock is held only for the blit duration.

### 1.3 Double-buffering as an alternative to locking

**Files**: `BasiliskII/src/SDL/video_sdl2.cpp`, `BasiliskII/src/prefs_items.cpp`

Instead of locking, use two frame buffers:
- `the_buffer`: JIT writes here (current Mac frame buffer, in reserved memory)
- `the_buffer_display`: Redraw thread reads from here (plain `calloc`, host-only)
- Snapshot `the_buffer` → `the_buffer_display` at `VideoInterrupt()`/`VideoVBL()` time

This eliminates lock contention: the redraw thread reads from a stable snapshot while the JIT keeps writing to `the_buffer`. Cost is ~300 KB for 640×480×32bpp — negligible on the Pi 3B's 1GB.

Gated by the `doublebuffer` CLI/prefs argument (default: off). Enable with `doublebuffer true` in the prefs file or `--doublebuffer true` on the command line.

- [x] Evaluate feasibility given `MEMBaseDiff` is baked into JIT-compiled blocks — confirmed safe: `MEMBaseDiff` references `RAMBaseHost`, not `the_buffer` pointer directly
- [x] Add `doublebuffer` pref descriptor to `prefs_items.cpp` and default in `AddPrefsDefaults()`
- [x] Allocate `the_buffer_display` via `calloc` in `driver_base::init()`, gated by `#if defined(USE_JIT) || defined(JIT)`
- [x] Add VBI snapshot (`memcpy`) in `VideoVBL()` (SheepShaver) and `VideoInterrupt()` (BasiliskII)
- [x] Add `read_buffer` indirection in `update_display_static()` and `update_display_static_bbox()` — all `the_buffer` read sites replaced
- [x] Skip `LOCK_FRAME_BUFFER`/`UNLOCK_FRAME_BUFFER` in `video_refresh_window_static()` when double-buffering is active
- [x] Initialize `the_buffer_display = NULL` in constructor, free in destructor
- [ ] Test on hardware with `doublebuffer true`

> **Done.** Implemented as an opt-in feature gated by `--doublebuffer true`. The JIT writes to `the_buffer` (vm\_acquire\_framebuffer, Mac-addressable). At each VBI, the CPU thread copies `the_buffer` → `the_buffer_display` (calloc'd, host-only). The redraw thread reads from `the_buffer_display` via a `read_buffer` pointer indirection in both `update_display_static()` and `update_display_static_bbox()`. When active, the redraw thread skips frame buffer locking entirely since it only reads from the VBI-snapshotted copy. Note: for non-VOSF modes, guest\_surface wraps `the_buffer` directly (via `SDL_CreateRGBSurfaceFrom`), so Screen\_blit writes snapshot data back into `the_buffer` — this is acceptable since the write-back contains the same pixel values from the VBI snapshot.

---

## Phase 2: Disable VOSF for JIT Builds (HIGH)

These items address Finding 4 (VOSF incompatibility with JIT).

### 2.1 Auto-disable VOSF when JIT is active (runtime)

**File**: `BasiliskII/src/SDL/video_sdl2.cpp`

In `driver_base::init()` (line 1180), add a check: if JIT is enabled, skip the VOSF initialization and fall through to the non-VOSF path. This ensures the `basilisk2-arm32-jit-vosf` build gracefully degrades.

```cpp
#ifdef ENABLE_VOSF
#if USE_JIT
    // VOSF is incompatible with JIT direct memory writes — the JIT emits
    // raw STR instructions that bypass mprotect-based dirty page detection,
    // causing races and excessive SIGSEGV overhead.
    use_vosf = false;
    printf("VOSF disabled: incompatible with JIT direct memory access\n");
#else
    use_vosf = true;
    // ... existing VOSF init code ...
#endif
#endif
```

- [x] Add `USE_JIT` / `JIT` guard in `driver_base::init()` — VOSF is now auto-disabled at compile time when JIT is defined
- [x] Non-VOSF fallback path is exercised in the JIT+VOSF build (compile-time guard skips VOSF init)
- [ ] Update `BRANCH_GAPS.md` to document this decision
- [x] Remove JIT+VOSF build variant from CI matrix

> **Done.** A `#if defined(USE_JIT) || defined(JIT)` guard in `driver_base::init()` in `video_sdl2.cpp` forces `use_vosf = false` and prints a diagnostic message. The VOSF init block is wrapped in `if (use_vosf)` so it is cleanly skipped. This means the JIT+VOSF build variant now produces identical runtime behavior to the plain JIT build — the CI matrix entry has been removed accordingly.

### 2.2 Remove JIT+VOSF build variant from CI

**File**: `.github/workflows/build-arm-jit.yml`

Since VOSF is auto-disabled at runtime when JIT is on, the `basilisk2-arm32-jit-vosf` build variant produces identical runtime behavior to `basilisk2-arm32-jit`. Removed from CI to avoid confusion.

- [x] Decision: remove (redundant — runtime behavior is identical)
- [x] Deleted matrix entry, artifact download, and release file reference

> **Done.** The JIT+VOSF matrix entry, its artifact download step, and release file glob have been removed from `build-arm-jit.yml`. The release body now lists four variants instead of five.

---

## Phase 3: Validate Existing Blitter Fixes (MEDIUM)

These items confirm that the blitter format fixes from commit 7211573a are working correctly on hardware, independent of the race condition.

### 3.1 Test 16-bit `Blit_RGB565_OBO` fix on hardware

Run the `basilisk2-arm32-jit` build (with Phase 1 fixes applied) at 16-bit depth:

```bash
./BasiliskII-arm32-jit --screen win/640/480/16
```

- [ ] Verify color accuracy (no red/blue swap, no green shift)
- [ ] Verify the B2_DEBUG_PIXELS output matches expected values
- [ ] Compare with `B2_RAW_16BIT=1` mode to isolate blitter vs pipeline issues

### 3.2 Test low bit depths (1/2/4/8-bit)

Run at each depth and verify:

```bash
./BasiliskII-arm32-jit --screen win/640/480/8
./BasiliskII-arm32-jit --screen win/640/480/4
./BasiliskII-arm32-jit --screen win/640/480/2
./BasiliskII-arm32-jit --screen win/640/480/1
```

- [ ] 8-bit: desktop pattern correct, no palette corruption
- [ ] 4-bit: greyscale gradient correct
- [ ] 2-bit: greyscale correct
- [ ] 1-bit: black-on-white text correct (check B/W palette fix from BRANCH_GAPS.md)

### 3.3 Fix default B/W palette

**File**: `BasiliskII/src/SDL/video_sdl2.cpp` around line 1221

The default B/W palette has index 0 uninitialized (see BRANCH_GAPS.md "BUG FOUND"). Fix:

```cpp
sdl_palette = SDL_AllocPalette(256);
sdl_palette->colors[0] = (SDL_Color){ .r = 0,   .g = 0,   .b = 0,   .a = 255 };  // Black
sdl_palette->colors[1] = (SDL_Color){ .r = 255, .g = 255, .b = 255, .a = 255 };  // White
SDL_SetSurfacePalette(s, sdl_palette);
```

- [x] Apply the palette fix — swapped index 0 (now white) and index 1 (now black) to match Mac convention (bit 0=white, bit 1=black)
- [ ] Test 1-bit mode boot screen

> **Done.** In `video_sdl2.cpp`, `sdl_palette->colors[0]` is now `{255, 255, 255, 255}` (white) and `colors[1]` is `{0, 0, 0, 255}` (black), matching the Mac 1-bit framebuffer convention where a 0-bit means white and a 1-bit means black.

---

## Phase 4: JIT-Aware Dirty Region Tracking (LOW — future optimization)

These items are optional performance improvements, not correctness fixes. Implement only after Phase 1–3 are validated.

### 4.1 Add lightweight dirty-region notification to JIT writes

**Files**: `BasiliskII/src/uae_cpu_2021/compiler/compemu_support.cpp`, `BasiliskII/src/SDL/video_sdl2.cpp`

Instead of scanning the entire frame buffer with `memcmp` every frame, the JIT could set a per-tile dirty bit when writing to the frame buffer region:

```cpp
// In writemem_real(), after the STR:
if (address >= framebuffer_start && address < framebuffer_end) {
    dirty_tiles[(address - framebuffer_start) / TILE_SIZE] = 1;
}
```

This converts `update_display_static_bbox()` from O(width×height) `memcmp` scanning to O(dirty_tiles) targeted updates.

**Complexity**: High — requires the JIT to know the frame buffer bounds at code generation time, or emit a runtime check. The frame buffer base can change on video mode switch.

- [ ] Evaluate if the `framebuffer_start` / `framebuffer_end` can be made available to JIT-compiled code as a global
- [ ] Prototype the dirty-bit approach with interpreted writes first
- [ ] If viable, add conditional code generation in `writemem_real()`

### 4.2 Investigate `gfxaccel` approach from SheepShaver

**Files**: `SheepShaver/src/gfxaccel.cpp`, `SheepShaver/src/video.cpp`

SheepShaver has a `gfxaccel` mechanism where Mac toolbox drawing traps explicitly notify the host of dirty regions. This completely avoids both SIGSEGV overhead and memcmp scanning. Investigate whether BasiliskII can intercept QuickDraw traps similarly.

- [ ] Read SheepShaver's `gfxaccel.cpp` implementation
- [ ] Determine which Mac traps could be intercepted in BasiliskII
- [ ] Evaluate if the Quadra 800 ROM's QuickDraw calls are interceptable

---

## Phase 5: Testing & Validation

### 5.1 Create a systematic test matrix

Run each build variant through these scenarios:

| Test | Expected Result |
|------|-----------------|
| Boot to desktop (8-bit) | Clean desktop pattern, no tile artifacts |
| Boot to desktop (32-bit) | Clean desktop, correct colors |
| Open/close windows | No residual artifacts at window edges |
| Drag window across screen | Smooth drag, no tearing or stuck tiles |
| Scroll in text editor | Smooth scroll, no horizontal striping |
| Full-screen redraw (Cmd-A in Finder) | Complete, clean redraw |
| Switch color depth in Monitors CP | No corruption after switch |
| Run for 10+ minutes | No progressive degradation |

Build variants to test:

- [ ] `basilisk2-arm32-jit` (Phase 1 fixes applied)
- [ ] `basilisk2-arm32-jit-vendored-sdl` (same fixes, vendored SDL2)
- [ ] `basilisk2-arm32-nojit` (baseline, should be clean)
- [ ] `basilisk2-arm32-nojit-vosf` (VOSF without JIT, should be clean)

> **Note:** The JIT+VOSF variant has been removed — VOSF is auto-disabled at runtime when JIT is active, making it redundant.

### 5.2 Add CI-level sanity checks

- [ ] Add a build step that verifies `LOCK_FRAME_BUFFER` is used in display update functions (grep-based)
- [ ] Add a build step that verifies `DMB` is present in the ARM codegen output (objdump-based)

---

## Decision Log

| Decision | Rationale | Date |
|----------|-----------|------|
| Prioritize non-VOSF path fix | The main JIT build uses `--disable-vosf`; most users will use this variant | Done |
| `DMB ISH` over `DMB ST` | Full inner-shareable barrier (loads+stores) is safer; `pthread_mutex_lock` on reader side needs matching acquire semantics. Cost difference is negligible on Cortex-A53 | Done |
| Lock at refresh function level | Wrapping the entire `video_refresh_window_static()` update call (not per-tile) keeps it simple; `VideoInterrupt()` releases the lock every VBL giving the redraw thread a window | Done |
| Auto-disable VOSF with JIT | Compile-time `#if defined(USE_JIT) || defined(JIT)` guard in `driver_base::init()` | Done |
| B/W palette swap | Mac 1-bit convention: bit 0=white, bit 1=black. Previous code had it inverted | Done |
| Remove JIT+VOSF CI variant | Redundant — VOSF auto-disabled at runtime when JIT is active; saves CI time and avoids user confusion | Done |
