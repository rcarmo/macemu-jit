# BasiliskII AArch64 JIT — Status Report

## Current State (2026-04-12)

**Build:** ✅ Compiles cleanly with `--enable-aarch64-jit-experimental`
**Interpreter:** ✅ Boots Mac OS desktop in ~45 seconds
**JIT:** ⚠️ Runs but blocked by dispatch overhead — never reaches disk drivers

### Commit History (Recent)

| Commit | Description |
|--------|-------------|
| `8b9787c5` | Revert experimental fast dispatch and big-block tracing |
| `449112e5` | ISP/MSP sync + bus error Exception(2) + 16MB ROM alloc + software renderer |
| `82c2de54` | ROM polling patches + JIT dispatch in m68k_execute + NuBus recovery |
| `863dd5e5` | MOVEM safe codegen |
| `9517d112` | Fix lea_l_brr brace duplication |
| `72a55f14` | Fix lea_l_brr 64-bit truncation |
| `8455f96a` | 64-bit PC_P eviction + MOVEM safe codegen + zero barriers |

### What Works
- JIT compiler initializes, compiles blocks, runs native ARM64 code
- ROM loads (Quadra 800, version 1660), XPRAM, storage drivers, video init all OK
- EMUL_OP_RESET correctly syncs ISP/MSP with a7
- NuBus slot probing handled via bus error Exception(2) — ROM handler skips empty slots
- ROM polling loops at $016a / $0172 patched to NOP

### Current Blocker: Block Transition Overhead

The JIT compiles blocks of 2-3 instructions (every branch = block boundary). Each block transition goes through:

```
compiled block → popall (JMP) → execute_normal (C++) → check_for_cache_miss → pushall → compiled block
```

This C++ round-trip on every branch makes tight loops ~100x slower than the interpreter. The ROM video fill loop at offset 0x7120 (copies 640×480 pixels) never completes in JIT mode.

**Root cause:** The UAE JIT compiler marks all branch/jump opcodes with `COMP_OPCODE_ISJUMP`, which forces a block boundary at every `BEQ`, `BNE`, `DBF`, etc. With `MAXRUN=64`, blocks can be up to 64 sequential (non-branch) instructions, but typical loop bodies contain branches every 2-7 instructions.

### Failed Approaches

1. **Fast native dispatch in popallspace** — Tried emitting ARM64 cache_tags lookup directly in the popall trampoline. Crashed due to ARM64 immediate encoding bugs (TBNZ/CBNZ).

2. **C++ fast dispatch loop in execute_normal** — Called `pushall_call_handler()` from a loop in `execute_normal`. Stack corruption because `popall` tail-jumps back to `execute_normal` instead of returning, losing the C call frame.

3. **Big-block tracing (loop unrolling)** — Continued tracing past internal branches to build larger blocks. Crashed because the compiler's branch handlers emit block-ending code (jump to popall) that corrupts the code stream when more instructions follow.

### What Needs to Happen

**Option A: Native dispatch loop in popallspace (recommended)**
Replace `popall_execute_normal` with native ARM64 code that:
1. Loads `regs.pc_p`
2. Looks up `cache_tags[pc_p & TAGMASK].handler`
3. If cached: jump directly to handler (no C++ overhead)
4. If uncached: fall through to C++ `execute_normal` for compilation
5. Periodically check `spcflags` for interrupt delivery

This needs correct ARM64 instruction encoding — the previous attempt had bugs in TBNZ/CBNZ branch offset patching.

**Option B: Compiler support for internal branches**
Teach `compile_block` to emit internal branch targets instead of block exits for branches whose target PC is within the traced block. Requires:
- Marking pc_hist entries as branch targets
- Emitting native conditional branches to label offsets within the block
- Updating the liveness/flags analysis to handle non-linear control flow

**Option C: Hybrid — interpreter for init, JIT for steady state**
Run the interpreter for the first N million instructions (covering ROM init), then switch to JIT. The steady-state Mac OS idle loop is simpler and would benefit from JIT.

## Test Environment
- ROM: Quadra800.ROM (1MB, version 1660)
- Disk: HD200MB (Mac OS 7.x)
- Display: Xvfb :99, SDL software renderer, 640×480
- Config: `jit true`, `jitcachesize 8192`, `modelid 14`, `cpu 4`, `fpu true`
