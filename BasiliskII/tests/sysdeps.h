/*
 * Minimal sysdeps.h for blitter unit tests.
 * This is intentionally small and does not depend on config.h.
 */

#ifndef SYSDEPS_H
#define SYSDEPS_H

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>

#define USE_SDL_VIDEO 0

typedef std::uint8_t uint8;
typedef std::int8_t int8;
typedef std::uint16_t uint16;
typedef std::int16_t int16;
typedef std::uint32_t uint32;
typedef std::int32_t int32;
typedef std::uint64_t uint64;
typedef std::int64_t int64;

typedef std::uintptr_t uintptr;
typedef std::intptr_t intptr;

#define VAL64(a) (a##LL)
#define UVAL64(a) (a##ULL)

#endif /* SYSDEPS_H */
