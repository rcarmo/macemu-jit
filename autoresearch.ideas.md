# Autoresearch Ideas — AArch64 JIT

## Completed
- ✅ Score=100 achieved: cpu_compatible=false + optlev capped at 1 (native block dispatch, interpreter opcodes)
- ✅ Native block dispatch (cache_tags lookup, endblock) is stable
- ✅ 36K-37K blocks compiled per 120s run

## Future optimizations (beyond score=100)
- **Enable native opcode compilation (optlev>1)**: The actual ARM64 opcode codegen has bugs causing SIGSEGV. Debug and fix specific opcodes:
  - Start with simplest opcodes (MOV, ADD, SUB) and enable one at a time
  - The crash in earlier testing was `LDR X4,[X4,#48]` at JIT cache+0x1250 after 7 blocks
  - Blocks that crashed included BEQ, MOVEQ, JMP(A5) — likely control flow codegen issues
  - DBcc already has a workaround forcing optlev=0 for blocks containing it
- **Investigate register save/restore on interpreter calls**: X6-X18 are used for m68k register allocation but are caller-saved in AArch64 ABI. The flush()/init_comp() cycle handles this, but verify correctness at native↔interpreter boundaries.
- **Profile JIT block compilation overhead**: 37K blocks compiled in 120s. Measure compilation time vs execution time.
- **Add optional (env-gated) allocator diagnostics**: Print first few vm_acquire() addresses and whether each is below 4GB, then disable once confidence is high.
