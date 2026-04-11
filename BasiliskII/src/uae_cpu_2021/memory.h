/*
 * UAE - The Un*x Amiga Emulator
 *
 * memory management
 *
 * Copyright 1995 Bernd Schmidt
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifndef UAE_MEMORY_H
#define UAE_MEMORY_H

#include "registers.h"

#if DIRECT_ADDRESSING
extern uintptr MEMBaseDiff;
#endif

extern void Exception (int, uaecptr);

static __inline__ bool trace_write_window_enabled(void)
{
    static int cached = -1;
    if (cached < 0)
        cached = (getenv("B2_TRACE_WRITE_START") && *getenv("B2_TRACE_WRITE_START")) ? 1 : 0;
    return cached != 0;
}

static __inline__ uae_u32 trace_write_window_start(void)
{
    static uae_u32 value = 0;
    static bool init = false;
    if (!init) {
        const char *env = getenv("B2_TRACE_WRITE_START");
        value = env && *env ? (uae_u32)strtoul(env, NULL, 0) : 0;
        init = true;
    }
    return value;
}

static __inline__ uae_u32 trace_write_window_end(void)
{
    static uae_u32 value = 0xffffffff;
    static bool init = false;
    if (!init) {
        const char *env = getenv("B2_TRACE_WRITE_END");
        value = env && *env ? (uae_u32)strtoul(env, NULL, 0) : 0xffffffff;
        init = true;
    }
    return value;
}

static __inline__ unsigned long trace_write_limit(void)
{
    static unsigned long value = 200;
    static bool init = false;
    if (!init) {
        const char *env = getenv("B2_TRACE_WRITE_LIMIT");
        value = env && *env ? strtoul(env, NULL, 0) : 200;
        init = true;
    }
    return value;
}

static __inline__ void trace_write_log(const char *kind, uaecptr addr, uae_u32 val)
{
    static unsigned long count = 0;
    if (!trace_write_window_enabled())
        return;
    if (addr < trace_write_window_start() || addr > trace_write_window_end())
        return;
    if (count >= trace_write_limit())
        return;
    count++;
    fprintf(stderr, "TRACEWRITE %s step=%lu pc=%08x addr=%08x val=%08x\n",
        kind, count, (unsigned)regs.fault_pc, (unsigned)addr, (unsigned)val);
}

/* Auto-select longjmp-based exceptions when C++ exceptions are disabled
   (e.g. -fno-exceptions). GCC/Clang undefine __EXCEPTIONS in that case. */
#if defined(__GNUC__) && !defined(__EXCEPTIONS) && !defined(EXCEPTIONS_VIA_LONGJMP)
#define EXCEPTIONS_VIA_LONGJMP 1
#endif

#ifdef EXCEPTIONS_VIA_LONGJMP
    #include <setjmp.h>
    #ifndef JMP_BUF
    #define JMP_BUF  sigjmp_buf
    #define SETJMP(env) sigsetjmp(env, 0)
    #define LONGJMP  siglongjmp
    #endif
    extern JMP_BUF excep_env;
    #define SAVE_EXCEPTION \
        JMP_BUF excep_env_old; \
        memcpy(excep_env_old, excep_env, sizeof(JMP_BUF))
    #define RESTORE_EXCEPTION \
        memcpy(excep_env, excep_env_old, sizeof(JMP_BUF))
    #define TRY(var) int var = SETJMP(excep_env); if (!var)
    #define CATCH(var) else
    #define THROW(n) LONGJMP(excep_env, n)
    #define THROW_AGAIN(var) LONGJMP(excep_env, var)
    #define VOLATILE volatile
#else
    struct m68k_exception {
        int prb;
        m68k_exception (int exc) : prb (exc) {}
        operator int() { return prb; }
    };
    #define SAVE_EXCEPTION
    #define RESTORE_EXCEPTION
    #define TRY(var) try
    #define CATCH(var) catch(m68k_exception var)
    #define THROW(n) throw m68k_exception(n)
    #define THROW_AGAIN(var) throw
    #define VOLATILE
#endif /* EXCEPTIONS_VIA_LONGJMP */

#if DIRECT_ADDRESSING
static __inline__ uae_u8 *do_get_real_address(uaecptr addr)
{
	return (uae_u8 *)MEMBaseDiff + addr;
}
static __inline__ uae_u32 do_get_virtual_address(uae_u8 *addr)
{
	return (uintptr)addr - MEMBaseDiff;
}
static __inline__ uae_u32 get_long(uaecptr addr)
{
    uae_u32 * const m = (uae_u32 *)do_get_real_address(addr);
    return do_get_mem_long(m);
}
#define phys_get_long get_long
static __inline__ uae_u32 get_word(uaecptr addr)
{
    uae_u16 * const m = (uae_u16 *)do_get_real_address(addr);
    return do_get_mem_word(m);
}
#define phys_get_word get_word
static __inline__ uae_u32 get_byte(uaecptr addr)
{
    uae_u8 * const m = (uae_u8 *)do_get_real_address(addr);
    return do_get_mem_byte(m);
}
#define phys_get_byte get_byte
static __inline__ void put_long(uaecptr addr, uae_u32 l)
{
    trace_write_log("L", addr, l);
    uae_u32 * const m = (uae_u32 *)do_get_real_address(addr);
    do_put_mem_long(m, l);
}
#define phys_put_long put_long
static __inline__ void put_word(uaecptr addr, uae_u32 w)
{
    trace_write_log("W", addr, w);
    uae_u16 * const m = (uae_u16 *)do_get_real_address(addr);
    do_put_mem_word(m, w);
}
#define phys_put_word put_word
static __inline__ void put_byte(uaecptr addr, uae_u32 b)
{
    trace_write_log("B", addr, b);
    uae_u8 * const m = (uae_u8 *)do_get_real_address(addr);
    do_put_mem_byte(m, b);
}
#define phys_put_byte put_byte
static __inline__ uae_u8 *get_real_address(uaecptr addr)
{
	return do_get_real_address(addr);
}
static inline uae_u8 *get_real_address(uaecptr addr, int write, int sz)
{
    return do_get_real_address(addr);
}
static inline uae_u8 *phys_get_real_address(uaecptr addr)
{
    return do_get_real_address(addr);
}
static __inline__ uae_u32 get_virtual_address(uae_u8 *addr)
{
	return do_get_virtual_address(addr);
}
#endif /* DIRECT_ADDRESSING */

static __inline__ void check_ram_boundary(uaecptr addr, int size, bool write) {}
static inline void flush_internals() {}

#endif /* MEMORY_H */

