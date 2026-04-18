/*
 *  ppc-jit-glue.hpp — Integration glue for AArch64 JIT in kpx_cpu execute loop
 *
 *  Include this from ppc-cpu.cpp when USE_AARCH64_JIT is defined.
 *  Provides try_jit_execute() which attempts to JIT-compile and execute
 *  the current block natively, returning true if successful.
 */

#ifndef PPC_JIT_AARCH64_GLUE_HPP
#define PPC_JIT_AARCH64_GLUE_HPP

#ifdef __aarch64__

#include "cpu/jit/aarch64/ppc-jit.h"

static bool jit_aarch64_initialized = false;

/* Try to JIT-compile and execute the block at current PC.
   Returns true if the block was executed natively (pc updated).
   Returns false if compilation failed (caller should interpret). */
static inline bool try_jit_execute(powerpc_cpu *cpu_obj, void *regs_ptr, uint32_t pc,
                                    const uint8_t *ram_base, size_t ram_size)
{
	if (!jit_aarch64_initialized) {
		if (!ppc_jit_aarch64_init(4096)) /* 4MB code cache */
			return false;
		jit_aarch64_initialized = true;
	}

	ppc_jit_block blk;
	if (!ppc_jit_aarch64_compile(pc, ram_base, ram_size, &blk))
		return false;

	if (!blk.complete)
		return false; /* Only execute fully-compiled blocks for safety */

	/* Call the generated native code */
	ppc_jit_entry_fn fn = (ppc_jit_entry_fn)(void *)blk.code;
	fn(regs_ptr);

	return true;
}

#endif /* __aarch64__ */
#endif /* PPC_JIT_AARCH64_GLUE_HPP */
