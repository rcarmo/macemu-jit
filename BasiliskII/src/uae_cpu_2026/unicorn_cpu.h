/*
 *  unicorn_cpu.h - 680x0 execution backend using the Unicorn engine
 *
 *  Basilisk II (C) 1997-2008 Christian Bauer
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifndef UNICORN_CPU_H
#define UNICORN_CPU_H

/*
 *  When built with USE_UNICORN_M68K, guest 68k code runs on Unicorn's
 *  TCG-based m68k translator.  The UAE register file stays the source of
 *  truth between runs: state is loaded into Unicorn before each run and
 *  stored back when it stops.
 *
 *  Unicorn never vectors 68k exceptions through the guest VBR; it reports
 *  them instead.  Every reported exception (EmulOps are illegal 0x71xx
 *  opcodes, A-line traps, TRAP #n, RTE, privilege violations, ...) is
 *  handled by executing that single instruction with the UAE interpreter,
 *  which builds the genuine 68k exception frame or runs the EmulOp.
 *  Interrupts are delivered by UAE's m68k_do_specialties() as before.
 *
 *  Set B2_USE_UNICORN=0 in the environment to use the UAE interpreter/JIT.
 */

#ifdef USE_UNICORN_M68K

// Set up the engine.  Returns true if Unicorn will execute guest code.
extern bool unicorn_cpu_init(void);
extern void unicorn_cpu_exit(void);

// True once unicorn_cpu_init() succeeded
extern bool unicorn_cpu_enabled;

// Replacement for m68k_do_execute(); returns when m68k_do_specialties() does
extern void unicorn_cpu_execute(void);

// Make a running unicorn_cpu_execute() return to the host; any thread
extern void unicorn_cpu_request_stop(void);

// Host code wrote guest memory: drop translations of [mac_addr, +size)
extern void unicorn_cpu_invalidate(uint32 mac_addr, uint32 size);
extern void unicorn_cpu_invalidate_all(void);

#else

static const bool unicorn_cpu_enabled = false;
static inline bool unicorn_cpu_init(void) { return false; }
static inline void unicorn_cpu_exit(void) { }
static inline void unicorn_cpu_execute(void) { }
static inline void unicorn_cpu_request_stop(void) { }
static inline void unicorn_cpu_invalidate(uint32, uint32) { }
static inline void unicorn_cpu_invalidate_all(void) { }

#endif

#endif /* UNICORN_CPU_H */
