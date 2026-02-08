# ARM JIT Branch - Fit/Gap Analysis

This document tracks the development status, known issues, and actionable items for the `feature/arm-jit` branch.

## Current Status

**Build**: ✅ Compiles successfully via GitHub Actions (ARM32 cross-compilation on Debian 12)  
**Runtime**: ⚠️ Runs but has display corruption issues  
**Fixes applied**:
- 🔧 RGB565 blitter fix (commit 7211573a) — awaiting hardware validation
- 🔧 JIT video corruption remediation — DMB ISH barriers, frame buffer locking, VOSF auto-disable, B/W palette fix (see [JIT_FINDINGS.md](JIT_FINDINGS.md) and [TODO.md](TODO.md))

## Known Issues

### 1. Screen Corruption (High Priority)

**Symptom**: Bitmap corruption visible on screen, significantly worse at lower bit depths (1/2/4/8-bit modes).

**Observations**:

- 32-bit color depth shows minimal/no corruption
- 16-bit shows moderate corruption
- 8-bit and below shows severe corruption
- The corruption pattern suggests issues with pixel format conversion or pitch calculations

**Root Cause Candidates**:

| Area                                      | Likelihood | Notes                                                                                                                                   |
| ----------------------------------------- | ---------- | --------------------------------------------------------------------------------------------------------------------------------------- |
| **JIT/redraw thread data race**           | **CONFIRMED** | JIT emits raw ARM `STR` to `the_buffer` with no locking; redraw thread reads concurrently via `memcmp`/`memcpy`. **Fixed**: DMB ISH barriers + frame buffer locking. See [JIT_FINDINGS.md](JIT_FINDINGS.md) |
| **Missing ARM memory barriers**           | **CONFIRMED** | No `DMB`/`DSB` in JIT codegen; stores invisible across cores on Cortex-A53 SMP. **Fixed**: `DMB_ISH()` at all six `popall_*` exit stubs |
| **VOSF incompatible with JIT**            | **CONFIRMED** | `mprotect`-based dirty tracking broken by JIT direct writes. **Fixed**: auto-disabled via compile-time guard |
| Screen_blit format conversion             | HIGH       | Mac is big-endian, ARM is little-endian. The blitters in `video_blit.cpp` may not handle all depth/format combinations correctly on ARM |
| Pitch mismatch in update_display_static   | MEDIUM     | The `update_display_static()` and `update_display_static_bbox()` functions have separate code paths for low bit depths vs 8+ bits       |
| SDL_UpdateTexture vs guest_surface format | MEDIUM     | Recent change from `SDL_LockTexture` to `SDL_UpdateTexture` may have format assumptions                                                 |
| JIT memory byte-swap operations           | LOW        | ARM JIT uses `REV`/`REV16`/`REVSH` instructions for byte swapping - these appear correct                                                |
| Texture pixel format mismatch             | MEDIUM     | Texture is BGRA8888, but guest surfaces vary by depth                                                                                   |

---

## Fit Analysis: What Works

### ARM JIT Compiler

- ✅ Successfully ported from ARAnyM
- ✅ ARM32 codegen with `REV` instructions for byte swapping
- ✅ Direct addressing mode enabled
- ✅ Signal handling configured for ARM cross-compilation
- ⚠️ **Integer-only JIT** — FPU operations use interpreted IEEE emulation (see [Future Work](#future-work-fpu-jit))

### SDL2 Video Backend

- ✅ KMSDRM support with OpenGL ES 2.0
- ✅ Window creation and texture rendering
- ✅ Basic frame refresh loop
- ✅ Mouse capture support (with evdev fallback)

### Input Handling

- ✅ evdev fallback for KMSDRM (when SDL capture unavailable)
- ✅ Keyboard input via SDL
- ✅ Mouse motion events

---

## Gap Analysis: Known Deficiencies

### Display Pipeline Issues

#### Gap 1: Low Bit Depth Pixel Expansion

**Location**: [video_sdl2.cpp](BasiliskII/src/SDL/video_sdl2.cpp#L927-L967), [video_blit.cpp](BasiliskII/src/CrossPlatform/video_blit.cpp)

**Problem**: 1/2/4-bit Mac depths expand to 8-bit SDL surfaces, then blit to 32-bit textures. The expansion and palette application path may have endianness issues.

**Code path**:

```
Mac framebuffer (1/2/4/8-bit) → guest_surface (8-bit paletted)
    → SDL_BlitSurface → host_surface (32-bit BGRA)
    → SDL_UpdateTexture → GPU texture
```

**Investigation needed**:

- Verify `Blit_Expand_*_To_8()` functions handle ARM little-endian correctly
- Check palette application in `update_palette()`
- Confirm SDL_BlitSurface color conversion is correct

#### Gap 2: 16-bit Color Pixel Format — 🔧 FIX APPLIED

**Location**: [video_sdl2.cpp#L944-L946](BasiliskII/src/SDL/video_sdl2.cpp#L944-L946), [video_blit.cpp#L212-L240](BasiliskII/src/CrossPlatform/video_blit.cpp#L212-L240)

**Problem**: Mac 16-bit is RGB555 big-endian, SDL surface is RGB565. Two bugs found:

1. `native_byte_order=true` was passed to `Screen_blitter_init()`, selecting NBO blitter
2. `Blit_RGB565_OBO` formula was marked "untested" and was completely wrong

**Fix applied** (commit 7211573a):
- Changed `native_byte_order` to `false` on little-endian hosts
- Rewrote `Blit_RGB565_OBO` formula with correct bit extraction (validated with Python)

**Status**: ⏳ Awaiting hardware test to confirm fix

#### Gap 3: VOSF on ARM — ⚠️ INCOMPATIBLE WITH JIT

**Location**: [configure.ac#L1503](BasiliskII/src/Unix/configure.ac#L1503), [video_sdl2.cpp driver_base::init()](BasiliskII/src/SDL/video_sdl2.cpp)

**Analysis**: VOSF (Video On SEGV Fault) CAN be enabled on ARM if signal handling is available. However, **VOSF is fundamentally incompatible with JIT direct memory writes**:

- The JIT emits raw ARM `STR` instructions (`canbang=1`) that bypass `mprotect`-based dirty page detection
- Every JIT write to a re-protected video page triggers a SIGSEGV → handler → `mprotect` cycle (~4,500 signals/sec at 60fps for a 640×480×8bpp framebuffer)
- Race window between `PFLAG_CLEAR_RANGE` and `Screen_blit` in `update_display_window_vosf()` causes missed updates
- See [JIT_FINDINGS.md §4](JIT_FINDINGS.md) for full analysis

**Resolution**: VOSF is now **auto-disabled at compile time** when JIT is defined via a `#if defined(USE_JIT) || defined(JIT)` guard in `driver_base::init()`. The JIT+VOSF CI build variant has been removed as redundant.

**CI Build Matrix** (updated):
| Build | JIT | VOSF | SDL2 |
|-------|-----|------|------|
| `basilisk2-arm32-jit` | ✅ | ❌ (auto-disabled) | System |
| `basilisk2-arm32-jit-vendored-sdl` | ✅ | ❌ (auto-disabled) | Vendored |
| `basilisk2-arm32-nojit` | ❌ | ❌ | System |
| `basilisk2-arm32-nojit-vosf` | ❌ | ✅ | System |

**VOSF benefits** (non-JIT only): Uses memory protection to detect dirty pages, only updates changed regions of framebuffer.

**Status**: ✅ VOSF works for non-JIT builds; auto-disabled for JIT builds

#### Gap 4: ROM Patching (JIT vs Non-JIT)

**Location**: [rom_patches.cpp](BasiliskII/src/rom_patches.cpp)

**Analysis**: Verified that `rom_patches.cpp` has NO JIT-specific conditionals. ROM patching is identical for JIT and non-JIT builds.

Both `uae_cpu/basilisk_glue.cpp` and `uae_cpu_2021/basilisk_glue.cpp` include the same `rom_patches.h`.

**Status**: ✅ Validated - no ROM patching differences

#### Gap 5: Monochrome and Greyscale Modes (1/2/4-bit depths)

**Location**: [video_blit.cpp#L331-L390](BasiliskII/src/CrossPlatform/video_blit.cpp#L331-L390), [video_sdl2.cpp#L2746-L2800](BasiliskII/src/SDL/video_sdl2.cpp#L2746-L2800)

**Problem**: Low bit depth modes (1-bit monochrome, 2-bit greyscale, 4-bit greyscale/color) use a different code path with potential endianness issues.

**Pipeline for 1/2/4-bit modes**:
```
Mac framebuffer (1/2/4-bit packed)
    → Blit_Expand_*_To_8() expands to 8-bit indices
    → ExpandMap[] lookup converts index → SDL pixel value
    → SDL_BlitSurface → host_surface (32-bit BGRA)
    → SDL_UpdateTexture → GPU texture
```

**Key functions in video_blit.cpp**:
| Function | Input | Output | Notes |
|----------|-------|--------|-------|
| `Blit_Expand_1_To_8` | 1 byte (8 pixels) | 8 bytes (8 indices) | Bit extraction MSB-first |
| `Blit_Expand_2_To_8` | 1 byte (4 pixels) | 4 bytes (4 indices) | 2-bit extraction MSB-first |
| `Blit_Expand_4_To_8` | 1 byte (2 pixels) | 2 bytes (2 indices) | 4-bit extraction MSB-first |

**`ExpandMap[]` initialization** ([video_sdl2.cpp#L2049](BasiliskII/src/SDL/video_sdl2.cpp#L2049)):
```c
ExpandMap[i] = SDL_MapRGB(drv->s->format, pal[c*3+0], pal[c*3+1], pal[c*3+2]);
```

**Potential issues on ARM little-endian**:

1. **Bit extraction order**: `Blit_Expand_1_To_8` extracts MSB-first (`c >> 7` first), which is correct for Mac's big-endian pixel ordering. This should be endian-neutral since it operates byte-by-byte.

2. **ExpandMap type mismatch**: `ExpandMap` is `uint32` but `Blit_Expand_*_To_8` writes `uint8` indices. The subsequent `Blit_Expand_*_To_16/32` functions use `ExpandMap` as a lookup table. On 16-bit destination, this writes `uint16` values to memory — **potential endianness issue if destination expects different byte order**.

3. **Destination offset calculation** ([video_sdl2.cpp#L2795](BasiliskII/src/SDL/video_sdl2.cpp#L2795)):
   ```c
   int di = y1 * dst_bytes_per_row + x1;
   ```
   For `x1` in pixel units and destination in bytes, this assumes 1 byte per pixel for the expanded 8-bit surface. But `drv->s` is a 32-bit BGRA surface, so `x1` should be multiplied by `BytesPerPixel`.

4. **Screen_blit function selection**: For low bit depths, `Screen_blitter_init()` selects based on `native_byte_order` which we fixed to `false` for 16-bit. But for 8-bit paletted → 32-bit BGRA, which blitter is used? The `Blit_Expand_8_To_32` path goes through `ExpandMap[]` which already contains SDL-native pixel values.

**Testing needed**:
- Set Mac to 1-bit B&W mode and check if display inverts or garbles
- Set Mac to 2-bit/4-bit greyscale and check gradient rendering
- Check if `ExpandMap` values match expected SDL pixel format

**🐛 ~~BUG FOUND~~ ✅ FIXED: Default B/W palette**

Location: [video_sdl2.cpp#L1231](BasiliskII/src/SDL/video_sdl2.cpp#L1231)

**Problem** (was): Only index 1 was set to black; index 0 was uninitialized. Mac monochrome convention is bit 0 = WHITE, bit 1 = BLACK. `Blit_Expand_1_To_8` writes raw bit values as palette indices.

**Fix applied**:
```c
// set default B/W palette (Mac convention: bit 0=white, bit 1=black)
sdl_palette = SDL_AllocPalette(256);
sdl_palette->colors[0] = (SDL_Color){ .r = 255, .g = 255, .b = 255, .a = 255 };  // White (Mac bit 0)
sdl_palette->colors[1] = (SDL_Color){ .r = 0,   .g = 0,   .b = 0,   .a = 255 };  // Black (Mac bit 1)
SDL_SetSurfacePalette(s, sdl_palette);
```

**Status**: ✅ Fixed — awaiting 1-bit mode hardware test

**Note**: The MacOS Monitors control panel should send proper palette data via `set_palette()` which will correctly populate `ExpandMap[]` and set the SDL palette. But the **default** palette used before MacOS initializes video is wrong.

**Debug approach**:
```bash
# Enable pixel debugging
export B2_DEBUG_PIXELS=1
export B2_DEBUG_VIDEO=1
```

---

## Actionable Issues

### High Priority

1. ~~**[BUG]** Add diagnostic mode to dump pixel data before/after Screen_blit~~ ✅ DONE

   - Added `B2_DEBUG_PIXELS` env var (commit 7211573a)
   - Added blitter selection logging to `Screen_blitter_init()`

2. ~~**[BUG]** Verify 16-bit format conversion~~ ✅ FIX APPLIED

   - Fixed `native_byte_order` parameter in video_sdl2.cpp
   - Fixed `Blit_RGB565_OBO` formula in video_blit.cpp
   - Validated with Python test suite (all colors pass)

3. **[BUG]** Test with raw memcpy for low bit depths

   - Location: video_sdl2.cpp `update_display_static()`
   - Action: Bypass Screen_blit and use direct memcpy to isolate issue
   - Existing debug: `B2_RAW_16BIT` env var exists for 16-bit

4. **[BUG]** Verify palette application path
   - Location: video_sdl2.cpp `update_palette()`
   - Action: Log palette entries and verify SDL palette is set correctly

### Medium Priority

5. **[FEATURE]** Add comprehensive video debug logging

   - Existing: `B2_DEBUG_VIDEO` env var
   - Action: Extend to log pixel format at each stage of pipeline

6. **[INVESTIGATE]** Compare master branch video path

   - The master branch uses the same video_sdl2.cpp but without ARM JIT
   - Test if corruption occurs with `--disable-jit-compiler`

7. **[INVESTIGATE]** Test with software renderer

   - Bypass OpenGL ES entirely
   - Use `SDL_HINT_RENDER_DRIVER=software`

8. **[REFACTOR]** Consider reverting SDL_LockTexture change
   - Recent commit 3b448b3c changed from `SDL_LockTexture` to `SDL_UpdateTexture`
   - May have introduced format assumptions

### Low Priority

9. **[CLEANUP]** Remove dead debug code from present_sdl_video()

   - Commit 3b448b3c removed some debug logging
   - Some g\_\* debug variables remain unused

10. **[DOCS]** Document build requirements and test procedure
    - Target hardware: Raspberry Pi 3B, 1GB RAM, 640x480 display
    - ROM: Quadra 800 (Model ID 35, ROM_VERSION_32)

---

## Debug Environment Variables

| Variable          | Purpose                                        |
| ----------------- | ---------------------------------------------- |
| `B2_DEBUG_VIDEO`  | Enable video pipeline logging                  |
| `B2_DEBUG_PIXELS` | Dump pixel values before/after blit (16-bit)   |
| `B2_DEBUG_INPUT`  | Enable evdev input logging                     |
| `B2_RAW_16BIT`    | Bypass Screen_blit for 16-bit mode             |
| `B2_EVDEV_MOUSE` | Override evdev mouse device path   |

---

## Blitter Test Suite

A comprehensive Python test suite validates all blitter formulas without requiring hardware:

```bash
python3 BasiliskII/src/CrossPlatform/test_blitters.py
```

### Test Results Summary

| Blitter | Status | Notes |
|---------|--------|-------|
| `Blit_RGB555_NBO` | ✅ PASS | Byte swap only |
| `Blit_RGB565_OBO` | ✅ PASS | Fixed in commit 7211573a |
| `Blit_RGB888_NBO` | ✅ PASS | 32-bit byte swap |
| `Blit_BGR555_NBO` | ✅ PASS | Marked "untested" in code |
| `Blit_BGR555_OBO` | ✅ PASS | Marked "untested" in code |
| `Blit_BGR888_NBO` | ❌ FAIL | Bug in formula (not used on SDL2) |
| `Blit_BGR888_OBO` | ❌ FAIL | Bug in formula (not used on SDL2) |

### BGR888 Bug Analysis

The `Blit_BGR888_NBO` formula (LE) is broken:
```c
dst = ((src) & 0xff00ff) | (((src) & 0xff00) << 16)
```

For white (src=0xFFFFFF00), this produces 0xFFFF0000 instead of expected 0x00FFFFFF.

**Impact**: Low — SDL2 uses BGRA8888 texture format, which uses `Blit_RGB888_NBO` (passes).
BGR blitters are for unusual display configurations (BGR pixel order) not common on modern hardware.

---

## CI Build Matrix

The CI workflow builds four variants:

| Variant | JIT | VOSF | SDL2 | Use Case |
|---------|-----|------|------|----------|
| `basilisk2-arm32-jit` | ✅ Enabled | ❌ Auto-disabled | System | Main release build |
| `basilisk2-arm32-jit-vendored-sdl` | ✅ Enabled | ❌ Auto-disabled | From source | Matches original build config |
| `basilisk2-arm32-nojit` | ❌ Disabled | ❌ | System | Isolate JIT vs video bugs |
| `basilisk2-arm32-nojit-vosf` | ❌ Disabled | ✅ Enabled | System | Test VOSF path (non-JIT only) |

The Python blitter tests run on every push as a separate job.

**Note**: The JIT+VOSF build variant was removed — VOSF is auto-disabled at runtime when JIT is active, making it redundant. See [Gap 3](#gap-3-vosf-on-arm--️-incompatible-with-jit).

---

## Tasks That Don't Require Hardware

The following can be done purely through code analysis and CI builds:

### Code Analysis & Fixes

| Task | Difficulty | Impact | Status |
|------|------------|--------|--------|
| Fix RGB565 OBO blitter formula | Medium | High | ✅ DONE (commit 7211573a) |
| Fix `native_byte_order` parameter | Easy | High | ✅ DONE (commit 7211573a) |
| Create Python blitter validation suite | Medium | High | ✅ DONE |
| Audit BGR555 blitters | Medium | Low | ✅ TESTED (pass) |
| Fix BGR888 NBO/OBO blitters | Medium | Low | 🔲 BUGS FOUND (not used on SDL2) |
| Audit `Blit_Expand_*_To_*` functions for endianness | Medium | High | 🔲 Not started |
| Static analysis with cppcheck/clang-tidy | Easy | Low | 🔲 Not started |
| Review pitch calculations in update_display_static | Medium | High | 🔲 Not started |

### Testing Infrastructure

| Task | Difficulty | Impact | Status |
|------|------------|--------|--------|
| Unit tests for pixel format conversions | Medium | High | ✅ DONE (`test_blitters.py`) |
| CI matrix for ARM32 build variants | Easy | Medium | ✅ DONE |
| CI job for Python blitter tests | Easy | Medium | ✅ DONE |
| Add build with `--disable-jit` for comparison | Easy | Medium | ✅ DONE (matrix variant) |
| Add build with vendored SDL2 | Medium | Medium | ✅ DONE (matrix variant) |
| Headless test mode (no display) | Hard | Low | 🔲 Not started |

### Documentation & Cleanup

| Task | Difficulty | Impact | Status |
|------|------------|--------|--------|
| Document all debug env vars | Easy | Medium | 🔲 Not started |
| Remove dead code from video_sdl2.cpp | Easy | Low | 🔲 Not started |
| Add architecture diagram for video pipeline | Medium | Medium | 🔲 Not started |

### Next Recommended Actions (No Hardware Needed)

1. ~~**Audit other OBO blitters**~~ ✅ DONE — BGR555 passes, BGR888 has bugs (not used on SDL2)
2. ~~**Write Python validation** for all `Blit_*` formulas~~ ✅ DONE (`test_blitters.py`)
3. ~~**Add CI build variant** with JIT disabled~~ ✅ DONE (3-way matrix: JIT/system SDL, JIT/vendored SDL, No-JIT/system SDL)
4. **Review `update_display_static()`** for low bit depths — different code path than 8+ bits
5. **Test the RGB565 fix on hardware** — commit 7211573a awaiting validation

---

## Build Configuration

Current ARM32 JIT build flags:

```
--enable-sdl-video --enable-sdl-audio --enable-jit-compiler
--enable-addressing=direct --enable-fpe=ieee --disable-vosf
--disable-gtk --without-mon --without-x --without-esd
--disable-nls --with-sdl2
```

---

## Historical Notes

### Old Manual Build (Pre-ARM JIT)

The original build used a vendored SDL2 with specific configuration:

```bash
# SDL2 2.30.0 is used in CI (2.32.8 does not exist - likely typo in original notes)
wget https://github.com/libsdl-org/SDL/releases/download/release-2.30.0/SDL2-2.30.0.tar.gz
tar -zxvf SDL2-2.30.0.tar.gz
cd SDL2-2.30.0 && ./configure --disable-video-opengl --disable-video-x11 \
    --disable-pulseaudio --disable-esd --disable-video-wayland && make -j4

cd macemu/BasiliskII/src/Unix && NO_CONFIGURE=1 ./autogen.sh && \
./configure --enable-sdl-audio --enable-sdl-framework --enable-sdl-video \
    --disable-vosf --without-mon --without-esd --without-gtk \
    --disable-jit-compiler --disable-nls
```

Note: This old build **disabled JIT** and used a custom SDL2 without OpenGL.

**Important**: Target is ARM32 (armhf), not ARM64. Even on 64-bit Raspberry Pi OS, the build uses `arm-linux-gnueabihf-gcc` cross-compiler.

---

## Future Work: FPU JIT

The current ARM JIT only accelerates integer 68k operations. Floating-point instructions (FADD, FMUL, FDIV, FSQRT, etc.) fall back to the interpreted IEEE FPU emulator (`fpu/fpu_ieee.cpp`).

### Why No ARM FPU JIT?

The ARAnyM project (source of this JIT) only implemented FPU JIT for x86/x86-64 using x87 stack-based instructions. The ARM architecture requires a completely different approach:

| Aspect | x86 FPU JIT | ARM FPU JIT (needed) |
|--------|-------------|----------------------|
| **Register model** | x87 stack (ST0-ST7) | VFP/NEON registers (D0-D31) |
| **Precision** | 80-bit extended | 64-bit double max |
| **Rounding modes** | x87 control word | FPSCR register |
| **Code generator** | `compemu_fpp.cpp` exists | Would need new `codegen_arm_fpu.cpp` |

### Implementation Notes

A future FPU JIT for ARM would require:

1. **New codegen file** (`codegen_arm_fpu.cpp`) with VFP/NEON instruction emission
2. **Register allocator extension** to manage D0-D31 float registers
3. **68881/68882 semantics mapping** to VFP operations
4. **Precision handling** — 68k uses 80-bit extended; ARM VFP is 64-bit double
5. **Exception flag mapping** between 68k and ARM FPSCR

### Performance Impact

For Mac applications with heavy FPU use (CAD, spreadsheets, QuickDraw GX), the interpreted FPU is a bottleneck. JIT-compiled integer code runs at near-native speed, but FPU operations remain ~10-50× slower.

**Priority**: Medium-Low (most classic Mac apps are integer-heavy)

---

## Related Branches

- `master`: UAE CPU interpreter (stable, no JIT on ARM)
- `feature/unicorn-cpu`: Unicorn Engine backend (QEMU TCG JIT)
- `feature/arm-jit`: ARM32 JIT (current focus) ← **this branch**
