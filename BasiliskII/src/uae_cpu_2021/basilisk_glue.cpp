/*
 *  basilisk_glue.cpp - Glue UAE CPU to Basilisk II CPU engine interface
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

#include "sysdeps.h"

#include "cpu_emulation.h"
#include "main.h"
#include "prefs.h"
#include "emul_op.h"
#include "rom_patches.h"
#include "timer.h"
#include "m68k.h"
#include "memory.h"
#include "readcpu.h"
#include "newcpu.h"
#include "compiler/compemu.h"

#include <string.h>

static bool trace_d6_enabled_glue()
{
	static int cached = -1;
	if (cached < 0)
		cached = (getenv("B2_TRACE_D6") && *getenv("B2_TRACE_D6")) ? 1 : 0;
	return cached != 0;
}

static bool trace_irqmanaged_env_glue()
{
	static int cached = -1;
	if (cached < 0)
		cached = (getenv("B2_TRACE_IRQMANAGED") && *getenv("B2_TRACE_IRQMANAGED") && strcmp(getenv("B2_TRACE_IRQMANAGED"), "0") != 0) ? 1 : 0;
	return cached != 0;
}

// RAM and ROM pointers
uint32 RAMBaseMac = 0;		// RAM base (Mac address space) gb-- initializer is important
uint8 *RAMBaseHost;			// RAM base (host address space)
uint32 RAMSize;				// Size of RAM
uint32 ROMBaseMac;			// ROM base (Mac address space)
uint8 *ROMBaseHost;			// ROM base (host address space)
uint32 ROMSize;				// Size of ROM

#if !REAL_ADDRESSING
// Mac frame buffer
uint8 *MacFrameBaseHost;	// Frame buffer base (host address space)
uint32 MacFrameSize;		// Size of frame buffer
int MacFrameLayout;			// Frame buffer layout
#endif

#if DIRECT_ADDRESSING
uintptr MEMBaseDiff;		// Global offset between a Mac address and its Host equivalent
#endif

#if USE_JIT
bool UseJIT = false;
#endif

static bool deferred_irq_env()
{
	static int cached = -1;
	if (cached < 0) {
		const char *managed = getenv("B2_JIT_MANAGED_IRQ");
		const char *deferred = getenv("B2_JIT_DEFER_IRQ");
		cached = ((managed && *managed && strcmp(managed, "0") != 0) ||
			(deferred && *deferred && strcmp(deferred, "0") != 0)) ? 1 : 0;
	}
	return cached != 0;
}

static uint32 deferred_irq_flags = 0;
static bool deferred_irq_active = false;

// #if defined(ENABLE_EXCLUSIVE_SPCFLAGS) && !defined(HAVE_HARDWARE_LOCKS)
B2_mutex *spcflags_lock = NULL;
// #endif

// From newcpu.cpp
extern int quit_program;


/*
 *  Initialize 680x0 emulation, CheckROM() must have been called first
 */

bool Init680x0(void)
{
	spcflags_lock = B2_create_mutex();
#if REAL_ADDRESSING
	// Mac address space = host address space
	RAMBaseMac = (uintptr)RAMBaseHost;
	ROMBaseMac = (uintptr)ROMBaseHost;
#elif DIRECT_ADDRESSING
	// Mac address space = host address space minus constant offset (MEMBaseDiff)
	// NOTE: MEMBaseDiff is set up in main_unix.cpp/main()
	RAMBaseMac = 0;
	ROMBaseMac = Host2MacAddr(ROMBaseHost);
#else
	// Initialize UAE memory banks
	RAMBaseMac = 0;
	switch (ROMVersion) {
		case ROM_VERSION_64K:
		case ROM_VERSION_PLUS:
		case ROM_VERSION_CLASSIC:
			ROMBaseMac = 0x00400000;
			break;
		case ROM_VERSION_II:
			ROMBaseMac = 0x00a00000;
			break;
		case ROM_VERSION_32:
			ROMBaseMac = 0x40800000;
			break;
		default:
			return false;
	}
	memory_init();
#endif

	init_m68k();
#if USE_JIT
	UseJIT = compiler_use_jit();
	fprintf(stderr, "JIT: UseJIT=%d\n", (int)UseJIT);
	if (UseJIT)
	    compiler_init();
#endif
	return true;
}


/*
 *  Deinitialize 680x0 emulation
 */

void Exit680x0(void)
{
#if USE_JIT
    if (UseJIT)
	compiler_exit();
#endif
	exit_m68k();
}


/*
 *  Initialize memory mapping of frame buffer (called upon video mode change)
 */

void InitFrameBufferMapping(void)
{
#if !REAL_ADDRESSING && !DIRECT_ADDRESSING
	memory_init();
#endif
}

/*
 *  Reset and start 680x0 emulation (doesn't return)
 */

void Start680x0(void)
{
	m68k_reset();
#if USE_JIT
    if (UseJIT)
	m68k_compile_execute();
    else
#endif
	m68k_execute();
}


/*
 *  Trigger interrupt
 */

void TriggerInterrupt(void)
{
	idle_resume();
	SPCFLAGS_SET( SPCFLAG_INT );
	if (UseDeferredInterruptModel() && trace_irqmanaged_env_glue()) {
		static unsigned long trace_count = 0;
		if (trace_count < 4000) {
			MakeSR();
			fprintf(stderr, "IRQM trigger %lu pc=%08x sr=%04x intmask=%u spc=%08x live=%08x latched=%08x active=%d\n",
				++trace_count,
				(unsigned)m68k_getpc(),
				(unsigned)regs.sr,
				(unsigned)regs.intmask,
				(unsigned)regs.spcflags,
				(unsigned)InterruptFlags,
				(unsigned)deferred_irq_flags,
				deferred_irq_active ? 1 : 0);
		}
	}
}

void TriggerNMI(void)
{
	//!! not implemented yet
	// SPCFLAGS_SET( SPCFLAG_BRK ); // use _BRK for NMI
}

bool UseDeferredInterruptModel(void)
{
#if USE_JIT
	return UseJIT && deferred_irq_env();
#else
	return false;
#endif
}

uint32 ConsumeDeferredInterruptFlags(void)
{
	uint32 flags = deferred_irq_flags;
	deferred_irq_flags = 0;
	deferred_irq_active = false;
	if (trace_irqmanaged_env_glue()) {
		static unsigned long trace_count = 0;
		if (trace_count < 4000) {
			MakeSR();
			fprintf(stderr, "IRQM consume %lu pc=%08x sr=%04x intmask=%u spc=%08x take=%08x live=%08x\n",
				++trace_count,
				(unsigned)m68k_getpc(),
				(unsigned)regs.sr,
				(unsigned)regs.intmask,
				(unsigned)regs.spcflags,
				(unsigned)flags,
				(unsigned)InterruptFlags);
		}
	}
	if (InterruptFlags)
		SPCFLAGS_SET(SPCFLAG_INT);
	return flags;
}

/*
 *  Get 68k interrupt level
 */

int intlev(void)
{
	if (!UseDeferredInterruptModel())
		return InterruptFlags ? 1 : 0;

	if (deferred_irq_active) {
		if (trace_irqmanaged_env_glue()) {
			static unsigned long busy_count = 0;
			if (busy_count < 4000) {
				MakeSR();
				fprintf(stderr, "IRQM intlev busy %lu pc=%08x sr=%04x intmask=%u spc=%08x live=%08x latched=%08x\n",
					++busy_count,
					(unsigned)m68k_getpc(),
					(unsigned)regs.sr,
					(unsigned)regs.intmask,
					(unsigned)regs.spcflags,
					(unsigned)InterruptFlags,
					(unsigned)deferred_irq_flags);
			}
		}
		return 0;
	}

	if (regs.intmask >= 1) {
		if (trace_irqmanaged_env_glue()) {
			static unsigned long masked_count = 0;
			if (masked_count < 4000) {
				MakeSR();
				fprintf(stderr, "IRQM intlev masked %lu pc=%08x sr=%04x intmask=%u spc=%08x live=%08x\n",
					++masked_count,
					(unsigned)m68k_getpc(),
					(unsigned)regs.sr,
					(unsigned)regs.intmask,
					(unsigned)regs.spcflags,
					(unsigned)InterruptFlags);
			}
		}
		return InterruptFlags ? 1 : 0;
	}

	const uint32 flags = ConsumeInterruptFlags();
	if (trace_irqmanaged_env_glue()) {
		static unsigned long sample_count = 0;
		if (sample_count < 4000) {
			MakeSR();
			fprintf(stderr, "IRQM intlev sample %lu pc=%08x sr=%04x intmask=%u spc=%08x sampled=%08x live_after=%08x\n",
				++sample_count,
				(unsigned)m68k_getpc(),
				(unsigned)regs.sr,
				(unsigned)regs.intmask,
				(unsigned)regs.spcflags,
				(unsigned)flags,
				(unsigned)InterruptFlags);
		}
	}
	if (!flags)
		return 0;

	deferred_irq_flags = flags;
	deferred_irq_active = true;
	if (trace_irqmanaged_env_glue()) {
		static unsigned long accept_count = 0;
		if (accept_count < 4000) {
			MakeSR();
			fprintf(stderr, "IRQM intlev accept %lu pc=%08x sr=%04x intmask=%u spc=%08x latched=%08x\n",
				++accept_count,
				(unsigned)m68k_getpc(),
				(unsigned)regs.sr,
				(unsigned)regs.intmask,
				(unsigned)regs.spcflags,
				(unsigned)deferred_irq_flags);
		}
	}
	return 1;
}


/*
 *  Execute MacOS 68k trap
 *  r->a[7] and r->sr are unused!
 */

void Execute68kTrap(uint16 trap, struct M68kRegisters *r)
{
	int i;

	if (trace_d6_enabled_glue())
		fprintf(stderr, "TRACE_D6 Execute68kTrap enter trap=%04x oldpc=%08x d6=%08x d7=%08x a4=%08x a5=%08x\n", trap, m68k_getpc(), r->d[6], r->d[7], r->a[4], r->a[5]);

	// Save old PC
	uaecptr oldpc = m68k_getpc();

	// Set registers
	for (i=0; i<8; i++)
		m68k_dreg(regs, i) = r->d[i];
	for (i=0; i<7; i++)
		m68k_areg(regs, i) = r->a[i];

	// Push trap and EXEC_RETURN on stack
	m68k_areg(regs, 7) -= 2;
	put_word(m68k_areg(regs, 7), M68K_EXEC_RETURN);
	m68k_areg(regs, 7) -= 2;
	put_word(m68k_areg(regs, 7), trap);

	// Execute trap
	m68k_setpc(m68k_areg(regs, 7));
	fill_prefetch_0();
	quit_program = 0;
	m68k_execute();

	// Clean up stack
	m68k_areg(regs, 7) += 4;

	// Restore old PC
	m68k_setpc(oldpc);
	fill_prefetch_0();

	// Get registers
	for (i=0; i<8; i++)
		r->d[i] = m68k_dreg(regs, i);
	for (i=0; i<7; i++)
		r->a[i] = m68k_areg(regs, i);
	if (trace_d6_enabled_glue())
		fprintf(stderr, "TRACE_D6 Execute68kTrap leave trap=%04x restorepc=%08x d6=%08x d7=%08x a4=%08x a5=%08x\n", trap, oldpc, r->d[6], r->d[7], r->a[4], r->a[5]);
	quit_program = 0;
}


/*
 *  Execute 68k subroutine
 *  The executed routine must reside in UAE memory!
 *  r->a[7] and r->sr are unused!
 */

void Execute68k(uint32 addr, struct M68kRegisters *r)
{
	int i;

	if (trace_d6_enabled_glue())
		fprintf(stderr, "TRACE_D6 Execute68k enter addr=%08x oldpc=%08x d6=%08x d7=%08x a4=%08x a5=%08x\n", addr, m68k_getpc(), r->d[6], r->d[7], r->a[4], r->a[5]);

	// Save old PC
	uaecptr oldpc = m68k_getpc();

	// Set registers
	for (i=0; i<8; i++)
		m68k_dreg(regs, i) = r->d[i];
	for (i=0; i<7; i++)
		m68k_areg(regs, i) = r->a[i];

	// Push EXEC_RETURN and faked return address (points to EXEC_RETURN) on stack
	m68k_areg(regs, 7) -= 2;
	put_word(m68k_areg(regs, 7), M68K_EXEC_RETURN);
	m68k_areg(regs, 7) -= 4;
	put_long(m68k_areg(regs, 7), m68k_areg(regs, 7) + 4);

	// Execute routine
	m68k_setpc(addr);
	fill_prefetch_0();
	quit_program = 0;
	m68k_execute();

	// Clean up stack
	m68k_areg(regs, 7) += 2;

	// Restore old PC
	m68k_setpc(oldpc);
	fill_prefetch_0();

	// Get registers
	for (i=0; i<8; i++)
		r->d[i] = m68k_dreg(regs, i);
	for (i=0; i<7; i++)
		r->a[i] = m68k_areg(regs, i);
	if (trace_d6_enabled_glue())
		fprintf(stderr, "TRACE_D6 Execute68k leave addr=%08x restorepc=%08x d6=%08x d7=%08x a4=%08x a5=%08x\n", addr, oldpc, r->d[6], r->d[7], r->a[4], r->a[5]);
	quit_program = 0;
}

void report_double_bus_error()
{
#if 0
	panicbug("CPU: Double bus fault detected !");
	/* would be cool to open SDL dialog here: */
	/* [Double bus fault detected. The emulated system crashed badly.
	    Do you want to reset ARAnyM or quit ?] [Reset] [Quit]"
	*/
	panicbug(CPU_MSG);
	CPU_ACTION;
#endif
}
