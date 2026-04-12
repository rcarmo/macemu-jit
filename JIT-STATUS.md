# BasiliskII AArch64 JIT — Status Report (2026-04-12)

## Current State

**Build:** ✅ Compiles cleanly with JIT enabled (`-DUSE_JIT -DJIT -DOPTIMIZED_FLAGS`)
**Binary:** 19MB ELF aarch64, dynamically linked
**Latest commit:** `863dd5e5` — MOVEM safe codegen

### What Works
- JIT compiler initializes and compiles blocks (2000 FT traces observed)
- ROM loads successfully (Quadra 800, version 1660)
- Video init, XPRAM, storage drivers — all OK
- PatchROM completes successfully
- Emulation starts and runs

### What Doesn't Work
- **Boot stalls in ROM polling loops** — execution never reaches Mac OS desktop
- **JIT dispatch may be falling back to interpreter** — billions of instructions run through `m68k_do_execute()` (NOJIT_DIAG output) rather than compiled blocks

## Current Blocker: ROM Polling Loops

Two ROM busy-wait loops prevent boot progression:

### Loop 1: Tick counter wait (ROM offset `0xc098`)
```
0xc092: move.l $016a.w,d0    ; load Ticks (VBL 60Hz counter)
0xc096: add.l  a0,d0          ; d0 = Ticks + delay
0xc098: cmp.l  $016a.w,d0    ; compare current Ticks
0xc09c: bgt.s  $-4            ; loop while d0 > Ticks
```
**Polls Mac low-memory $016a (Ticks)**, waiting for the VBL tick counter to advance.
This appears early in boot and consumes billions of interpreter instructions.

### Loop 2: Flag polling (ROM offset `0x2a38`)
```
0x2a38: tst.b  $0172          ; test byte at address $0172
0x2a3c: bne.s  $-4            ; loop while non-zero
```
**Polls Mac low-memory $0172**, waiting for a hardware flag to clear.
The system gets permanently stuck here (~1.885 billion instructions in).

## Architecture Issues

### 1. JIT vs Interpreter Execution Path
- `Start680x0()` correctly enters `m68k_compile_execute()` when UseJIT=true
- But `Execute68k()` and `Execute68kTrap()` (called from EMUL_OP handlers) always use `m68k_execute()` — the **interpreter**
- If EMUL_OP handlers trigger nested 68k execution that enters a polling loop, the interpreter spins forever
- This is likely the reason for massive interpreter instruction counts

### 2. Uncommitted Changes from Previous Session
A previous session had destructive uncommitted changes that:
- Replaced the entire `rom_patches.cpp` (1700 lines) with a 38-line stub
- Removed JIT defines from the Makefile
- Changed type definitions in `emul_op.h`
**These were discarded.** All useful changes were committed properly.

## Commit History (Recent)

| Commit | Description |
|--------|-------------|
| `863dd5e5` | MOVEM safe codegen — use writeword/readword instead of native buffer ops |
| `9517d112` | Fix duplicate if(s_is_d) brace in lea_l_brr 32-bit fallback |
| `72a55f14` | Fix lea_l_brr 64-bit truncation for PC_P |
| `8455f96a` | 64-bit PC_P eviction + MOVEM safe codegen + zero barriers |
| `4ad2b573` | Native EMUL_OP compiled handler — no EMUL_OP barrier |

## What Needs to Be Done

### Priority 1: Patch ROM Polling Loops
Add ROM patches in `rom_patches.cpp` (within the existing `PatchROM` infrastructure) to NOP out or bypass:
- **$016a tick wait** at ROM offset 0xc098 (4 bytes: `b0b8 016a` → `4e71 4e71`)
- **$0172 flag poll** at ROM offset 0x2a38 (4 bytes: `4a38 0172` + `66fa` → NOP sequence)

### Priority 2: Fix Execute68k JIT Dispatch
`Execute68k()` and `Execute68kTrap()` in `basilisk_glue.cpp` should use `m68k_do_compile_execute()` when UseJIT is true, not the interpreter. This would allow JIT-compiled code to run during EMUL_OP nested execution.

### Priority 3: Investigate JIT Block Execution
Even with patches, need to verify:
- Are compiled blocks actually executing or just being compiled?
- Does the JIT dispatch loop (`pushall_call_handler`) work correctly on AArch64?
- Are block chain transitions preserving state properly?

### Priority 4: Remove Remaining Barriers
Continue removing fallback containment and EMUL_OP barriers to allow fully native ARM64 JIT execution for all opcode families.

## Test Environment
- ROM: Quadra800.ROM (1MB, version 1660)
- Disk: HD200MB (200MB disk image with Mac OS)
- Display: Xvfb :99 (640x480)
- Config: `jit true`, `jitcachesize 8192`, `modelid 14`, `cpu 4`
