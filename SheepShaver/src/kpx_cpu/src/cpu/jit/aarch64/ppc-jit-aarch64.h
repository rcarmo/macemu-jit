/*
 *  ppc-jit-aarch64.h — PPC → AArch64 direct codegen JIT interface
 */

#ifndef PPC_JIT_AARCH64_H
#define PPC_JIT_AARCH64_H

#include <stdint.h>
#include <stddef.h>

#ifdef __aarch64__

struct powerpc_registers;

struct ppc_jit_block {
	uint32_t *code;
	size_t    code_size;
	uint32_t  ppc_start_pc;
	uint32_t  ppc_end_pc;
	int       n_insns;
	bool      complete;
};

bool ppc_jit_aarch64_init(size_t cache_size_kb);
void ppc_jit_aarch64_exit(void);
void ppc_jit_aarch64_flush(void);

bool ppc_jit_aarch64_compile(
	uint32_t pc,
	const uint8_t *ram,
	size_t ramsize,
	ppc_jit_block *out
);

typedef void (*ppc_jit_entry_fn)(void *regs);

#endif /* __aarch64__ */
#endif /* PPC_JIT_AARCH64_H */
