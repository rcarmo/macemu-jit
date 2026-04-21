#ifndef JIT_NATIVE_HELPERS_H
#define JIT_NATIVE_HELPERS_H

/* Forward declarations for JIT native-call helpers.
 * These functions are implemented in compemu_legacy_arm64_compat.cpp
 * and called via compemu_raw_call from JIT-compiled blocks. */

#if defined(CPU_aarch64) || defined(CPU_AARCH64)

extern "C" {

/* SR/CCR operations */
void jit_op_MakeFromSR(void);
void jit_op_MakeSR(void);
void jit_op_orsr(void);
void jit_op_andsr(void);
void jit_op_eorsr(void);

/* MOVEC */
void jit_op_movec2(void);
void jit_op_move2c(void);

/* Divide */
void jit_op_divu_w(void);
void jit_op_divs_w(void);
void jit_op_mull(void);
void jit_op_divl(void);

/* BCD */
void jit_op_abcd(void);
void jit_op_sbcd(void);
void jit_op_nbcd(void);

/* Privileged/flow */
void jit_op_mvr2usp(void);
void jit_op_mvusp2r(void);
void jit_op_reset(void);
void jit_op_rte(void);
void jit_op_rtr(void);
void jit_op_stop(void);
void jit_op_trap(void);
void jit_op_trapv(void);
void jit_op_chk(void);

/* Bit operations */
void jit_op_tas(void);
void jit_op_bfins(void);

/* Rotate/shift */
void jit_op_roxl(void);
void jit_op_roxr(void);

/* Memory shift/rotate */
void jit_op_asrw(void);
void jit_op_aslw(void);
void jit_op_lsrw(void);
void jit_op_lslw(void);
void jit_op_rolw(void);
void jit_op_rorw(void);
void jit_op_roxlw(void);
void jit_op_roxrw(void);

/* Cache */
void jit_op_cinva(void);
void jit_op_cpusha(void);

} /* extern "C" */

#endif /* CPU_aarch64 */
#endif /* JIT_NATIVE_HELPERS_H */

/* ARM_CCR_MAP: M68K NZVC→ARM NZCV flag translation table (defined in compemu_midfunc_arm64_2.cpp) */
extern const uae_u32 ARM_CCR_MAP[];

/* JIT FPU shadow register sync */
#ifdef USE_JIT_FPU
void jit_fpu_sync_to_shadow(void);
void jit_fpu_sync_from_shadow(void);
#endif

