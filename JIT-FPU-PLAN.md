# JIT FPU Implementation Plan — ARM64

## Current State

- **Interpreter FPU**: `FPU_MPFR` — 80-bit extended precision via GNU MPFR library
- **JIT FPU code**: `compemu_fpp.cpp` (2094 lines) — complete but disabled on ARM64
- **JIT FPU mid-layer**: ARM64 NEON/FP — `fadd_rr`, `fsub_rr`, `fmul_rr`, `fdiv_rr`, `fsqrt_rr`, `fabs_rr`, `fneg_rr`, `frndint_rr`, `fmod_rr`, etc. (42 MIDFUNC functions in `compemu_midfunc_arm64.cpp`)
- **Missing**: `-DUSE_JIT_FPU`, `compemu_fpp.o` not linked, type incompatibility

## Architecture

```
M68K FPU instruction
       │
       ├── Interpreter path (current): fpuop_arithmetic() → MPFR calls
       │   fpu.registers[i] = struct { mpfr_t f; ... }  (48+ bytes, 80-bit precision)
       │
       └── JIT path (to enable): comp_fpp_opp() → mid-layer → ARM64 NEON
            shadow_fpregs[i] = double  (8 bytes, 64-bit precision)
            ARM64 FADD/FSUB/FMUL/FDIV/FSQRT on D registers
```

## The Type Incompatibility

`fpu_register_address(i)` returns `&fpu.registers[i]`:

| Backend | `fpu_register` type | Size | Precision |
|---------|---------------------|------|-----------|
| `FPU_MPFR` | `struct { mpfr_t f; uint64 nan_bits; int nan_sign; }` | ~48 bytes | 80-bit |
| `FPU_IEEE` | `double` or `long double` | 8-16 bytes | 64/80-bit |

The JIT FP register allocator does `STR D_reg, [mem]` / `LDR D_reg, [mem]` — writing 8 bytes of IEEE double. If `mem` points into an `mpfr_t` struct, this corrupts the MPFR internal state.

## Solution: Shadow FP Register File

Add a shadow array of 8 `double` values for JIT use:

```c
// In regs or fpu struct:
double jit_fpregs[8];       // Shadow FP registers for JIT (64-bit double)
double jit_fp_result;        // Shadow FP_RESULT
```

Modify `fpu_register_address()` or the register allocator init to point at the shadow array instead of `fpu.registers[]`.

### Sync Points

**JIT block entry** (when FP regs are first accessed):
- Convert `fpu.registers[i].f` (mpfr_t) → `jit_fpregs[i]` (double)
- `mpfr_get_d(fpu.registers[i].f, MPFR_RNDN)`

**JIT block exit** (flush):
- Convert `jit_fpregs[i]` (double) → `fpu.registers[i].f` (mpfr_t)
- `mpfr_set_d(fpu.registers[i].f, jit_fpregs[i], MPFR_RNDN)`

**JIT → interpreter transition** (when a block can't compile an FPU op):
- Flush shadow → MPFR before interpreter runs

## Precision Trade-off

| Aspect | MPFR (current) | JIT FPU (proposed) |
|--------|----------------|-------------------|
| Precision | 80-bit extended | 64-bit double |
| Mantissa bits | 64 | 52 |
| Exponent range | ±16383 | ±1023 |
| Speed | ~500 ns/op (MPFR call) | ~2 ns/op (ARM64 FADD) |
| Speedup | 1× | ~250× |

### What breaks with 64-bit

- **Accumulation errors**: long chains of FP ops lose more precision
- **Extended-precision intermediates**: `(a*b + c*d)` computed in 80-bit is more accurate
- **Exponent overflow/underflow**: values near 10^±4932 (80-bit range) become ±inf in double
- **NaN payload bits**: 80-bit NaN has 63 payload bits, double has 51

### What works fine with 64-bit

- **Most Mac applications**: games, word processors, spreadsheets, graphics
- **SANE library calls**: Apple's own FP library normalizes to double anyway
- **Mac OS system code**: doesn't use extended precision
- **Photoshop, Illustrator, etc.**: use double-precision internally

This is the same trade-off macOS ARM64 makes. Apple's own Rosetta 2 translates x87 80-bit to ARM64 64-bit double.

## Implementation Steps

1. **Add shadow FP register array** to `regstruct` (`regs.jit_fpregs[8]`)
2. **Modify register allocator init** to point FP reg memory at shadow array
3. **Write sync functions**: `jit_fpu_sync_to_shadow()` / `jit_fpu_sync_from_shadow()`
4. **Add `-DUSE_JIT_FPU` to Makefile**
5. **Add `compemu_fpp.o` to link**
6. **Fix compile errors** in `compemu_fpp.cpp` (FPU_MPFR type refs if any)
7. **Add `jitfpu` pref** (default true)
8. **Test with FPU-using apps**

## Files to Modify

| File | Change |
|------|--------|
| `newcpu.h` | Add `jit_fpregs[8]` and `jit_fp_result` to `regstruct` |
| `compemu_support_arm.cpp` | Point FP reg allocator at shadow array |
| `compemu_fpp.cpp` | Fix any MPFR-specific type issues |
| `Makefile` | Add `-DUSE_JIT_FPU`, add `compemu_fpp.o` |
| `gencomp.c` | Already has FPU codegen under `USE_JIT_FPU` |

## Risk Assessment

- **Low risk**: The JIT FPU code exists and works on x86/macOS. ARM64 mid-layer is proven.
- **Medium risk**: Shadow sync at JIT↔interpreter boundaries could miss edge cases.
- **Low risk**: 64-bit precision is sufficient for virtually all Mac software.
- **Mitigation**: Keep `FPU_MPFR` interpreter as fallback. If a block has FPU ops that can't compile, they fall back to MPFR automatically.
