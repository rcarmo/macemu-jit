/*
 *  jit-target-cache.hpp — AArch64 JIT code cache management
 *
 *  AArch64 requires explicit cache maintenance after writing code:
 *    1. DC CVAU  — clean data cache line to Point of Unification
 *    2. DSB ISH  — ensure clean completes
 *    3. IC IVAU  — invalidate instruction cache line
 *    4. DSB ISH  — ensure invalidation completes
 *    5. ISB      — synchronize instruction stream
 */

#ifndef JIT_TARGET_CACHE_HPP
#define JIT_TARGET_CACHE_HPP

#include <stdint.h>
#include <sys/mman.h>

static inline void jit_cache_flush(void *start, size_t length) {
    uintptr_t addr = (uintptr_t)start;
    uintptr_t end = addr + length;

    /* AArch64 cache line size is typically 64 bytes, but query it at runtime
       for correctness. For now, use 64 as a safe default. */
    const uintptr_t line_size = 64;
    const uintptr_t line_mask = ~(line_size - 1);

    /* Clean data cache */
    for (uintptr_t a = addr & line_mask; a < end; a += line_size)
        __asm__ volatile("dc cvau, %0" :: "r"(a));

    __asm__ volatile("dsb ish");

    /* Invalidate instruction cache */
    for (uintptr_t a = addr & line_mask; a < end; a += line_size)
        __asm__ volatile("ic ivau, %0" :: "r"(a));

    __asm__ volatile("dsb ish");
    __asm__ volatile("isb");
}

static inline void *jit_cache_alloc(size_t size) {
    void *p = mmap(NULL, size,
                   PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return (p == MAP_FAILED) ? NULL : p;
}

static inline void jit_cache_free(void *p, size_t size) {
    if (p) munmap(p, size);
}

#endif /* JIT_TARGET_CACHE_HPP */
