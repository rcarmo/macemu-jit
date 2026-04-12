# BasiliskII AArch64 JIT — Bringup Status

## Current State (2026-04-12 22:45 UTC)

**Build:** ✅ `--enable-aarch64-jit-experimental`
**Interpreter:** ✅ Boots Mac OS 7.x desktop in ~45s
**JIT:** ⚠️ ROM boot reaches slot ROM init + video clear + ROM offset 0x4b32. Stuck in loop — likely another opcode codegen bug.

## Commit Log

| Commit | Description |
|--------|-------------|
| `5d945ce1` | **Fix DBF branch condition CC_CC→CC_CS** + remove spcflags from direct dispatch + tick interval 16 |
| `a7992537` | spcflags diagnostic + setvbuf for reliable log capture |
| `789254fc` | Add spcflags check to direct dispatch (later removed — always set) |
| `9059ecd5` | Fix side-exit REG_PC_TMP loading |
| `7dd85d12` | Big-block tracing with side exits + lazy cache flush |
| `449112e5` | ISP/MSP sync + bus error Exception(2) + 16MB ROM + software renderer |
| `82c2de54` | ROM polling patches + JIT dispatch + NuBus recovery |

## Key Bugs Found & Fixed

### 1. DBF branch condition inverted (CRITICAL)
**gencomp.c case 1 (DBF):** `register_branch(v1,v2,NATIVE_CC_CC)` — branches when carry clear (borrow = D0.W wrapped to -1). This is **inverted**: DBF should branch when D0.W ≠ -1 (no borrow = carry set = `NATIVE_CC_CS`). Every DBF loop ran infinitely.

**Root cause:** ARM64 SUB sets C=0 on borrow, C=1 on no-borrow (opposite of x86). The gencomp.c DBF handler was written for x86 semantics.

**Fix:** Changed `NATIVE_CC_CC` → `NATIVE_CC_CS` in both gencomp.c and gencomp_arm.c.

### 2. spcflags perpetually set
The 60Hz timer thread sets `SPCFLAG_INT` every 16ms. With spcflags checks in `jmp_pc_tag`/`endblock_pc_inreg`, **every block transition** took the slow path → defeated direct chaining entirely.

**Fix:** Removed per-dispatch spcflags checks. Mid-block tick injection (every 16 compiled instructions) calls `cpu_do_check_ticks()` which handles interrupts.

### 3. Side-exit REG_PC_TMP never loaded
Side-exit code called `compemu_raw_set_pc_i()` (stores to `regs.pc_p` via `REG_WORK1/x2`) then `compemu_raw_endblock_pc_inreg(REG_PC_TMP/x1)` which reads from `x1`. But `x1` was never loaded → garbage → `bad_pc_p=(nil)`.

**Fix:** `LOAD_U64(REG_PC_TMP, side_exit_pc)` before `endblock_pc_inreg`.

### 4. ISP/MSP not synced at reset
`EMUL_OP_RESET` set `r->a[7]` but not `regs.isp`/`regs.msp`. When the ROM's NuBus probe triggered bus errors in supervisor mode, `a7` was read from `regs.isp` (which was 0) → stack corruption → `a7=0x00000008`.

**Fix:** Set `regs.isp = regs.msp = r->a[7]` in EMUL_OP_RESET and `m68k_reset()`.

### 5. No bus error for unmapped PC
JIT tried to execute from NuBus addresses (past ROM allocation) without raising Exception(2). The ROM's bus error handler (installed at vector 0x0008) expects to catch these and skip empty slots.

**Fix:** Check Mac address validity in `execute_normal`. If PC outside RAM/ROM, raise `Exception(2, safe_pc)` with `a7` restored from ISP.

## Architecture

### Block Compilation Pipeline
1. **Trace loop** (`compemu_legacy_arm64_compat.cpp`): interpreter executes M68K instructions building `pc_hist[]`. Internal branches (target within ±512 bytes, blocklen < 48) continue tracing.
2. **compile_block** (`compemu_support_arm.cpp`): for each `pc_hist` entry, calls generated opcode handler. Branch handlers call `register_branch()` storing targets + condition.
3. **Mid-block side exits**: after each non-final branch, emit `B.cond` guard to exit stub. Exit stub loads alt PC into `REG_PC_TMP` and calls `endblock_pc_inreg`. Regalloc state saved/restored via `bigstate`.
4. **Tick injection**: every 16 compiled instructions, emit `flush → cpu_do_check_ticks → spcflags check → conditional block exit`.
5. **Final endblock**: `compemu_raw_jcc_l_oponly` + predicted/not-predicted paths with `endblock_pc_isconst`.

### Dispatch Flow
Compiled blocks chain directly via `cache_tags` lookup + `BR` in `jmp_pc_tag`/`endblock_pc_inreg`. No C++ overhead between cached blocks. Uncached blocks fall through to `popall_execute_normal` → `execute_normal()` for compilation.

### Important: gencomp.c is the active generator
On ARM64, `gencomp.c` (x86-origin) generates `compemu.cpp`, NOT `gencomp_arm.c`. The `#if !defined(CPU_AARCH64)` at line 554 of `compemu_support.cpp` excludes the main file's functions, but `gencomp.c` output is used for all opcode handlers. The ARM-specific `compemu_support_arm.cpp` (included at line 36) provides the dispatch infrastructure.

## Test Environment
- ROM: Quadra800.ROM (1MB, version 1660)  
- Disk: HD200MB (Mac OS 7.x, freshly extracted)
- Display: Xvfb :99, SDL software renderer, 640×480
- Config: `jit true`, `jitcachesize 8192`, `modelid 14`, `cpu 4`, `fpu true`
