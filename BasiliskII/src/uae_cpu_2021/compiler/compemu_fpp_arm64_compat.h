/* compemu_fpp_arm64_compat.h — ARM64 compatibility shims for compemu_fpp.cpp
 * Maps x86 FPU mid-layer names to ARM64 equivalents.
 */
#ifndef COMPEMU_FPP_ARM64_COMPAT_H
#define COMPEMU_FPP_ARM64_COMPAT_H

#if defined(CPU_aarch64) || defined(CPU_AARCH64)

#include <cmath>

/* No-op delay (x87 pipeline stall workaround, not needed on ARM64) */
static inline void nop(void) {}

/* Additional delay macro used in compemu_fpp.cpp */
#define delay

/* Word/byte memory access — use existing readword/readbyte/writeword/writebyte */
static inline void mov_w_rm(int dst, uintptr src) { mov_l_rm(dst, src); }
static inline void mov_w_mr(uintptr dst, int src) { mov_l_mr(dst, src); }
static inline void mov_b_rm(int dst, uintptr src) { mov_l_rm(dst, src); }

/* FP integer conversions (load int to FP, store FP to int) */
#define fmovi_rm(d, m) fmov_l_rr(d, m)
#define fmovi_mr(m, s) fmov_to_l_rr(m, s)

/* FP store to memory — use fp_from_double_mr for double format */
#define fmov_mr(m, s) fp_from_double_mr(m, s)
#define fmovs_mr(m, s) fmov_to_s_rr(m, s)

/* Extended (80-bit) format load/store — use double approximation */
#define fmov_ext_rm(d, m) fp_to_exten_rm(d, m)
#define fmov_ext_mr(m, s) fp_from_exten_mr(m, s)

/* FP constants */
#define fmov_0(d)       fmov_d_ri_0(d)
#define fmov_1(d)       fmov_d_ri_1(d)
#define fmov_pi(d)      fmov_s_ri(d, 0x40490FDB)  /* float pi */
#define fmov_log10_2(d) fmov_s_ri(d, 0x3E9A209B)  /* float log10(2) */
#define fmov_log2_e(d)  fmov_s_ri(d, 0x3FB8AA3B)  /* float log2(e) */
#define fmov_loge_2(d)  fmov_s_ri(d, 0x3F317218)  /* float ln(2) */

/* Transcendental functions — use ffunc_rr with C library functions */
#define fsin_rr(d, s)     ffunc_rr(sin, d, s)
#define fcos_rr(d, s)     ffunc_rr(cos, d, s)
#define fetox_rr(d, s)    ffunc_rr(exp, d, s)
#define flog2_rr(d, s)    ffunc_rr(log2, d, s)
#define ftwotox_rr(d, s)  fpowx_rr(2, d, s)

/* FREM — IEEE remainder */
#define frem_rr(d, s)     frem1_rr(d, s)

/* fflags_into_flags — ARM64 version takes no argument */
/* On x86 it takes a workspace register; on ARM64 it doesn't need one */
#define fflags_into_flags_adjusted() fflags_into_flags()

#endif /* CPU_aarch64 */
#endif /* COMPEMU_FPP_ARM64_COMPAT_H */

