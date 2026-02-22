# ARM JIT Debugging Skill (BasiliskII on Raspberry Pi)

## Purpose

Reusable process for diagnosing ARM32 JIT startup/crash issues in this repo (`feature/arm-jit`), with emphasis on:

- distinguishing JIT-entry faults from guest memory faults
- using SSH + GDB batch workflows on real Pi hardware
- applying low-risk fixes that preserve BasiliskII behavior

> Project guardrails: do not modify SheepShaver sources; target BasiliskII ARM32 behavior on Pi.

---

## Environment Baseline

- Target host: `pi@se30.local`
- Binary under test: `/home/pi/BasiliskII-arm32-jit`
- Typical prefs file: `/home/pi/.basilisk_ii_prefs`
- ROM profile used in testing: Quadra 800 (`modelid 35`, ROM32)

Always verify target connectivity first:

```bash
ssh -o BatchMode=yes -o ConnectTimeout=8 pi@se30.local 'echo ok && uname -m'
```

---

## Core Debugging Strategy

### 1) Classify failure stage first

Use logs and backtrace to quickly classify one of these stages:

1. **Pre-JIT init failure** (e.g., compiler table setup)
2. **JIT entry/trampoline failure** (`m68k_do_compile_execute`, `pushall_call_handler`)
3. **Opcode runtime failure** (e.g., `op_xxxx_0_ff` in `cpuemu.cpp`)
4. **Exception-loop failure** (repeated exception 2 / bus error)

This determines whether to inspect:

- JIT code generation/cache coherency (stage 2)
- guest register/effective-address semantics (stage 3)
- exception handling and fallback policy (stage 4)

### 2) Use deterministic GDB captures

Start with a baseline batch run:

```bash
ssh -o BatchMode=yes -o ConnectTimeout=10 pi@se30.local \
  "cd /home/pi && gdb -q -batch ./BasiliskII-arm32-jit \
     -ex 'set pagination off' \
     -ex run \
     -ex 'info program' \
     -ex 'bt full' \
     -ex 'info registers' \
     -ex 'x/30i \$pc-40'"
```

Then add targeted probes:

- breakpoint on suspect opcode (e.g., `op_4a28_0_ff`, `op_2068_0_ff`)
- conditional watchpoint for bad register patterns (e.g., `regs.regs[11]` entering `0x5xxxxxxx`)
- breakpoints after key loads/stores to inspect source address/value

### 3) Prove causality, not just symptom

For each bad value, identify:

- where it is first written
- source effective address and raw bytes
- whether source guest address is within valid Mac RAM/ROM/framebuffer ranges

Do not stop at downstream crash opcode if an earlier opcode injected corrupted register state.

---

## High-Value Findings Pattern (from this incident)

### A) Instruction-cache coherency on ARM

If JIT-generated ARM code is written to memory and then executed, instruction cache must be explicitly flushed.

- `mprotect()` alone is insufficient on ARM Linux.
- Missing flush after popall stub generation can crash immediately at compiled-entry.

### B) JIT logging readability matters

If log macros route to `printf` without newline, diagnostic output collapses into unreadable streams.

- Keep per-event line breaks.
- Avoid duplicating prefixes if call sites already include them.

### C) Trap loop vs hard crash

Converting SIGSEGV to exception 2 prevents hard crash, but can expose repeated fault loops.

- Add controlled fallback (disable JIT and resume interpreter) after repeated identical bus errors.

### D) Root cause in this case: out-of-range guest read

The bad A-register value was seeded by a `MOVEA`-style opcode (`op_2068_0_ff`) reading from guest address beyond configured RAM end.

- Example pattern observed:
  - RAM end: `RAMBaseMac + RAMSize = 0x08800000`
  - source guest addr: `0x088D36EC` (out-of-range)
  - loaded longword: `0x0040f150` -> endian-swapped into `0x50f14000`
  - later dereference causes SIGSEGV

---

## Fix Approach Used

### 1) Preserve Basilisk personality (do not define `UAE` globally)

`UAE` is a broad personality switch with many include-path and behavior changes. Avoid enabling globally.

### 2) Add Basilisk-safe JIT exception handling

Install signal-based guard for JIT compiled section so host faults become exception 2 instead of immediate process termination.

### 3) Add fallback policy

If identical bus errors repeat, disable JIT and fall back to interpreter to avoid infinite loops and final hard crash.

### 4) Add direct-address bounds checks carefully

Apply bounds checks to **actual memory accessors** (`get_long/word/byte`) to catch invalid guest reads.

Be careful with write/path helpers:

- Startup ROM patching writes through `WriteMacInt32` / `put_long`.
- Overly strict throw-on-write or throw-on-pointer-conversion can abort initialization via uncaught `m68k_exception`.

Guideline:

- keep strict read validation
- keep write/pointer conversion semantics compatible with ROM patching and existing startup flows

---

## Command Snippets (Reusable)

### Catch first throw origin

```bash
ssh -o BatchMode=yes -o ConnectTimeout=10 pi@se30.local \
  "cd /home/pi && gdb -q -batch ./BasiliskII-arm32-jit \
     -ex 'set pagination off' \
     -ex 'break __cxa_throw' \
     -ex run \
     -ex 'bt 30'"
```

### Catch first suspicious A3 write

```bash
ssh -o BatchMode=yes -o ConnectTimeout=10 pi@se30.local \
  "cd /home/pi && gdb -q -batch ./BasiliskII-arm32-jit \
     -ex 'set pagination off' \
     -ex 'set can-use-hw-watchpoints 1' \
     -ex 'watch -l regs.regs[11] if ((regs.regs[11] & 0xF0000000) == 0x50000000)' \
     -ex run \
     -ex 'bt 16' \
     -ex 'info registers'"
```

### Inspect source load in a specific opcode

```bash
# Example for op_2068_0_ff + 52 load
ssh -o BatchMode=yes -o ConnectTimeout=10 pi@se30.local \
  "cd /home/pi && gdb -q -batch ./BasiliskII-arm32-jit \
     -ex 'set pagination off' \
     -ex 'break op_2068_0_ff' \
     -ex run \
     -ex 'set \$entry=\$pc' \
     -ex 'tbreak *(\$entry+52)' \
     -ex continue \
     -ex 'info registers r0 r1 r2 r3 r4 r5 pc'"
```

---

## Decision Checklist

Before changing code, answer:

1. Is crash at JIT entry or at opcode runtime?
2. Is bad register value created earlier by valid emulation semantics, or by out-of-range memory access?
3. Is proposed validation located in accessor paths that are always inside CPU `TRY/CATCH`?
4. Could change break ROM patch/init writes?
5. Is fallback behavior deterministic and observable in logs?

---

## What “Good” Looks Like

- JIT starts and runs without immediate SIGSEGV.
- Invalid guest memory accesses become handled exception flow (or clean interpreter fallback), not host abort.
- Logs clearly show transitions (`JIT enabled`, exceptions, fallback trigger).
- No regressions in startup path (ROM patching still succeeds).

---

## Notes for Future Iteration

- If opcode-level bad addresses persist, instrument generated opcode handlers around source effective-address calculation and table reads.
- Consider adding optional debug-only asserts for guest address range at key JIT/interpretive read points.
- Keep fixes minimal and reversible; prioritize behavior parity with existing Basilisk paths.
