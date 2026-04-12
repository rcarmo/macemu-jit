# BasiliskII AArch64 JIT — Bringup Status

## Current State (2026-04-12 21:00 UTC)

**Build:** ✅ `--enable-aarch64-jit-experimental`
**Interpreter:** ✅ Boots Mac OS 7.x desktop in ~45s
**JIT:** ⚠️ Compiles and executes 48-49 instruction blocks natively. Zero crashes. Stuck in ROM init loop — dispatch overhead.

## What Works

- JIT compiler init, 46498 compileable opcodes
- Big-block tracing: internal branches continue trace (up to 48 insns)
- Side-exit guards for non-traced branch paths with regalloc save/restore
- ISP/MSP sync at EMUL_OP_RESET and m68k_reset
- Bus error Exception(2) for unmapped PC (NuBus probe)
- Software SDL renderer for headless Xvfb
- Lazy cache flush (no hard flushes during execution)
- 16MB ROM allocation covering NuBus probe address space

## Current Blocker

After 14 block compilations, the JIT enters a tight loop at ROM offset `0x9ab0` (data table init). Blocks of 48-49 instructions execute natively with zero errors, but the system never progresses to disk drivers.

The loop compiles repeatedly with slightly different traces (blocklen 48 vs 49), suggesting the loop body varies between iterations. Each block transition still goes through the full C++ `execute_normal()` → `check_for_cache_miss()` → `pushall_call_handler()` path.

## Commit Log

| Commit | Description |
|--------|-------------|
| `9059ecd5` | Fix side-exit REG_PC_TMP loading + clean write_jmp_target |
| `bda52044` | B.cond relay stubs (reverted) + block size mod-16 |
| `7dd85d12` | Big-block tracing with side exits + lazy cache flush |
| `904b890c` | Re-enable JIT_COMPILE logging |
| `449112e5` | ISP/MSP sync + bus error Exception(2) + 16MB ROM + software renderer |
| `82c2de54` | ROM polling patches + JIT dispatch + NuBus recovery |

## Architecture

### Block Compilation
1. Trace loop (`compemu_legacy_arm64_compat.cpp`): interpreter executes instructions, records `pc_hist[]`. Internal branches (target within ±512 bytes, blocklen < 48) continue tracing instead of ending the block.
2. `compile_block()`: for each `pc_hist` entry, calls the opcode's compiled handler. Branch handlers call `register_branch(not_taken, taken, cond)` but don't emit native branches.
3. After each mid-block branch instruction: side-exit guard emitted — `B.cond` to exit stub that loads alternate PC into `REG_PC_TMP` and calls `endblock_pc_inreg`.
4. Register allocator state saved/restored around side exits via `bigstate`.
5. Final block instruction: normal endblock with `compemu_raw_jcc_l_oponly` + predicted/not-predicted paths.

### Dispatch Flow
```
m68k_compile_execute() → pushall_call_handler() → compiled block
  → popall_execute_normal (JMP) → execute_normal() (C++)
  → check_for_cache_miss() → pushall_call_handler() → next block...
```

Each block transition does a full C++ function call round-trip. With 48 instruction blocks containing ~8 loop iterations each, the overhead is ~12% of execution time for tight loops.

## Test Environment
- ROM: Quadra800.ROM (1MB, version 1660)
- Disk: HD200MB (Mac OS 7.x, freshly extracted)
- Display: Xvfb :99, SDL software renderer, 640×480
- Config: `jit true`, `jitcachesize 8192`, `modelid 14`, `cpu 4`, `fpu true`
