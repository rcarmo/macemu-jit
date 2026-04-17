/*
 *  dyngen-target-exec.h — AArch64 target definitions for kpx_cpu JIT
 *
 *  Placeholder for Phase 2 (JIT scaffolding). Phase 1 uses interpreter only.
 */

#ifndef DYNGEN_TARGET_EXEC_H
#define DYNGEN_TARGET_EXEC_H

/* AArch64 host register assignments for PPC guest state.
   These are used by the JIT block dispatch and entry/exit stubs.
   Phase 1 (interpreter) does not use these. */

/* Callee-saved registers pinned to hot PPC GPRs */
#define REG_CPU_STATE   "x20"   /* powerpc_cpu state pointer */
#define REG_PPC_GPR0    "x19"
#define REG_PPC_GPR1    "x21"   /* PPC stack pointer — frequently accessed */
#define REG_PPC_GPR2    "x22"
#define REG_PPC_GPR3    "x23"   /* First function argument */
#define REG_PPC_GPR4    "x24"
#define REG_PPC_GPR5    "x25"
#define REG_PPC_GPR6    "x26"
#define REG_PPC_LR      "x27"
#define REG_PPC_CTR     "x28"

/* Scratch registers for codegen (caller-saved, freely clobberable) */
#define REG_WORK0       "x0"
#define REG_WORK1       "x1"
#define REG_WORK2       "x2"
#define REG_WORK3       "x3"
#define REG_WORK4       "x4"

#endif /* DYNGEN_TARGET_EXEC_H */
