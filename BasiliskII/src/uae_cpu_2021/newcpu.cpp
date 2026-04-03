/*
 * newcpu.cpp - CPU emulation
 *
 * Copyright (c) 2010 ARAnyM dev team (see AUTHORS)
 * 
 *
 * Inspired by Christian Bauer's Basilisk II
 *
 * This file is part of the ARAnyM project which builds a new and powerful
 * TOS/FreeMiNT compatible virtual machine running on almost any hardware.
 *
 * ARAnyM is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * ARAnyM is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with ARAnyM; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */
 /*
  * UAE - The Un*x Amiga Emulator
  *
  * MC68000 emulation
  *
  * (c) 1995 Bernd Schmidt
  */

#include "sysdeps.h"
#include <cassert>

#include "cpu_emulation.h"
#include "main.h"
#include "emul_op.h"
#include "m68k.h"
#include "memory.h"
#include "readcpu.h"
#include "newcpu.h"

extern "C" uaecptr basilisk_trace_fault_pc(void)
{
	return regs.fault_pc;
}

static bool trace_d6_enabled()
{
	static int cached = -1;
	if (cached < 0)
		cached = (getenv("B2_TRACE_D6") && *getenv("B2_TRACE_D6")) ? 1 : 0;
	return cached != 0;
}

static bool trace_irqmanaged_env()
{
	static int cached = -1;
	if (cached < 0)
		cached = (getenv("B2_TRACE_IRQMANAGED") && *getenv("B2_TRACE_IRQMANAGED") && strcmp(getenv("B2_TRACE_IRQMANAGED"), "0") != 0) ? 1 : 0;
	return cached != 0;
}

static bool trace_emulopflow_env()
{
	static int cached = -1;
	if (cached < 0)
		cached = (getenv("B2_TRACE_EMULOPFLOW") && *getenv("B2_TRACE_EMULOPFLOW") && strcmp(getenv("B2_TRACE_EMULOPFLOW"), "0") != 0) ? 1 : 0;
	return cached != 0;
}

static unsigned long trace_emulopflow_limit()
{
	static unsigned long value = 0;
	static bool init = false;
	if (!init) {
		const char *env = getenv("B2_TRACE_EMULOPFLOW_LIMIT");
		value = env && *env ? strtoul(env, NULL, 0) : 4000;
		init = true;
	}
	return value;
}

static unsigned long trace_emulopflow_count = 0;

static bool trace_emulopflow_opcode(uae_u32 opcode)
{
	return (opcode & 0xff00) == 0x7100;
}

static bool trace_window_enabled()
{
	static int cached = -1;
	if (cached < 0)
		cached = (getenv("B2_TRACE_PC_START") && *getenv("B2_TRACE_PC_START")) ? 1 : 0;
	return cached != 0;
}

static uae_u32 trace_window_start()
{
	static uae_u32 value = 0;
	static bool init = false;
	if (!init) {
		const char *env = getenv("B2_TRACE_PC_START");
		value = env && *env ? (uae_u32)strtoul(env, NULL, 0) : 0;
		init = true;
	}
	return value;
}

static uae_u32 trace_window_end()
{
	static uae_u32 value = 0xffffffff;
	static bool init = false;
	if (!init) {
		const char *env = getenv("B2_TRACE_PC_END");
		value = env && *env ? (uae_u32)strtoul(env, NULL, 0) : 0xffffffff;
		init = true;
	}
	return value;
}

static unsigned long trace_window_limit()
{
	static unsigned long value = 200;
	static bool init = false;
	if (!init) {
		const char *env = getenv("B2_TRACE_LIMIT");
		value = env && *env ? strtoul(env, NULL, 0) : 200;
		init = true;
	}
	return value;
}

static bool trace_window_matches(uae_u32 pc)
{
	return trace_window_enabled() && pc >= trace_window_start() && pc <= trace_window_end();
}

static void trace_window_log(const char *phase, unsigned long step, uae_u32 pc, uae_u32 opcode)
{
	MakeSR();
	fprintf(stderr,
		"TRACEWIN %s step=%lu pc=%08x op=%04x sr=%04x "
		"D0=%08x D1=%08x D2=%08x D3=%08x D4=%08x D5=%08x D6=%08x D7=%08x "
		"A0=%08x A1=%08x A2=%08x A3=%08x A4=%08x A5=%08x A6=%08x A7=%08x\n",
		phase,
		step,
		(unsigned)pc,
		(unsigned)opcode,
		(unsigned)regs.sr,
		(unsigned)m68k_dreg(regs, 0), (unsigned)m68k_dreg(regs, 1),
		(unsigned)m68k_dreg(regs, 2), (unsigned)m68k_dreg(regs, 3),
		(unsigned)m68k_dreg(regs, 4), (unsigned)m68k_dreg(regs, 5),
		(unsigned)m68k_dreg(regs, 6), (unsigned)m68k_dreg(regs, 7),
		(unsigned)m68k_areg(regs, 0), (unsigned)m68k_areg(regs, 1),
		(unsigned)m68k_areg(regs, 2), (unsigned)m68k_areg(regs, 3),
		(unsigned)m68k_areg(regs, 4), (unsigned)m68k_areg(regs, 5),
		(unsigned)m68k_areg(regs, 6), (unsigned)m68k_areg(regs, 7));
}

struct trace_recent_entry {
	unsigned long seq;
	uae_u32 pc;
	uae_u32 opcode;
	uae_u16 sr;
	uae_u32 d0, d1, d3, d6;
	uae_u32 a0, a3, a4, a5, a6;
};

static bool trace_recent_enabled()
{
	static int cached = -1;
	if (cached < 0)
		cached = (getenv("B2_TRACE_RING") && *getenv("B2_TRACE_RING")) ? 1 : 0;
	return cached != 0;
}

static unsigned trace_recent_window()
{
	static unsigned value = 64;
	static bool init = false;
	if (!init) {
		const char *env = getenv("B2_TRACE_RING_WINDOW");
		value = env && *env ? (unsigned)strtoul(env, NULL, 0) : 64;
		if (value == 0)
			value = 64;
		if (value > 2048)
			value = 2048;
		init = true;
	}
	return value;
}

static unsigned trace_recent_event_limit()
{
	static unsigned value = 8;
	static bool init = false;
	if (!init) {
		const char *env = getenv("B2_TRACE_RING_EVENTS");
		value = env && *env ? (unsigned)strtoul(env, NULL, 0) : 8;
		if (value == 0)
			value = 8;
		init = true;
	}
	return value;
}

static trace_recent_entry trace_recent_ring[2048];
static unsigned trace_recent_pos = 0;
static unsigned trace_recent_count = 0;
static unsigned long trace_recent_seq = 0;

static void trace_recent_record(uae_u32 pc, uae_u32 opcode)
{
	if (!trace_recent_enabled())
		return;
	MakeSR();
	trace_recent_entry &e = trace_recent_ring[trace_recent_pos];
	e.seq = ++trace_recent_seq;
	e.pc = pc;
	e.opcode = opcode;
	e.sr = regs.sr;
	e.d0 = m68k_dreg(regs, 0);
	e.d1 = m68k_dreg(regs, 1);
	e.d3 = m68k_dreg(regs, 3);
	e.d6 = m68k_dreg(regs, 6);
	e.a0 = m68k_areg(regs, 0);
	e.a3 = m68k_areg(regs, 3);
	e.a4 = m68k_areg(regs, 4);
	e.a5 = m68k_areg(regs, 5);
	e.a6 = m68k_areg(regs, 6);
	trace_recent_pos = (trace_recent_pos + 1) % 2048;
	if (trace_recent_count < 2048)
		trace_recent_count++;
}

extern "C" void basilisk_trace_dump_recent(const char *reason, uaecptr addr, uae_u32 value)
{
	static unsigned dumps = 0;
	if (!trace_recent_enabled() || dumps >= trace_recent_event_limit())
		return;
	dumps++;
	unsigned want = trace_recent_window();
	if (want > trace_recent_count)
		want = trace_recent_count;
	fprintf(stderr, "TRACEEVENT %u reason=%s addr=%08x value=%08x recent=%u\n",
		dumps, reason ? reason : "?", (unsigned)addr, (unsigned)value, want);
	unsigned start = (trace_recent_pos + 2048 - want) % 2048;
	for (unsigned i = 0; i < want; i++) {
		const trace_recent_entry &e = trace_recent_ring[(start + i) % 2048];
		fprintf(stderr,
			"TRACERING seq=%lu pc=%08x op=%04x sr=%04x D0=%08x D1=%08x D3=%08x D6=%08x A0=%08x A3=%08x A4=%08x A5=%08x A6=%08x\n",
			e.seq, (unsigned)e.pc, (unsigned)e.opcode, (unsigned)e.sr,
			(unsigned)e.d0, (unsigned)e.d1, (unsigned)e.d3, (unsigned)e.d6,
			(unsigned)e.a0, (unsigned)e.a3, (unsigned)e.a4, (unsigned)e.a5, (unsigned)e.a6);
	}
}
#ifdef USE_JIT
# include "compiler/compemu.h"
#endif
#include "fpu/fpu.h"
#include "cpummu.h"
#if 0
#include "natfeats.h"
#include "disasm-glue.h"
#endif
#if USE_JIT
extern bool UseJIT;
#endif

#include <cstdlib>

#define DEBUG 0
#include "debug.h"

#define SANITY_CHECK_ATC 1

struct fixup fixup = {0, 0, 0};

int quit_program = 0;
int exit_val = 0;

// For instruction $7139
bool cpu_debugging = false;

struct flag_struct regflags;

/* LongJump buffers */
#ifdef EXCEPTIONS_VIA_LONGJMP
JMP_BUF excep_env;
#endif
/* Opcode of faulting instruction */
uae_u16 last_op_for_exception_3;
/* PC at fault time */
uaecptr last_addr_for_exception_3;
/* Address that generated the exception */
uaecptr last_fault_for_exception_3;

int areg_byteinc[] = { 1,1,1,1,1,1,1,2 };
int imm8_table[] = { 8,1,2,3,4,5,6,7 };

int movem_index1[256];
int movem_index2[256];
int movem_next[256];

#ifdef FLIGHT_RECORDER

// feel free to edit the following defines to customize the dump
#define FRLOG_HOTKEY	1	/* 1 = dump only when hotkey is held down */
#define FRLOG_ALL	1	/* 1 = dump continuously to ever growing log */
#define FRLOG_IRQ	0	/* 1 = dump also CPU in interrupts */
#define FRLOG_REGS	0	/* 1 = dump also all data/address registers */
#define FRLOG_SIZE	8192	/* this many instructions in single dump */

struct rec_step {
	uae_u32 d[8];
	uae_u32 a[8];
	uae_u32 pc;
	uae_u16 sr;
	uae_u32 usp;
	uae_u32 msp;
	uae_u32 isp;
	uae_u16 instr;
};

bool cpu_flight_recorder_active = false;

#if FRLOG_ALL
const int LOG_SIZE = 10;
#else
const int LOG_SIZE = FRLOG_SIZE;
#endif
static rec_step frlog[LOG_SIZE];
static int log_ptr = -1; // First time initialization

static const char *log_filename(void)
{
	const char *name = getenv("M68K_LOG_FILE");
	return name ? name : "log.68k";
}

void dump_flight_recorder(void)
{
#if FRLOG_ALL
	FILE *f = fopen(log_filename(), "a");
#else
	FILE *f = fopen(log_filename(), "w");
#endif
	if (f == NULL)
		return;
	for (int i = 0; i < LOG_SIZE; i++) {
		int j = (i + log_ptr) % LOG_SIZE;
		fprintf(f, "pc %08x  instr %04x  sr %04x  usp %08x  msp %08x  isp %08x\n", frlog[j].pc, frlog[j].instr, frlog[j].sr, frlog[j].usp, frlog[j].msp, frlog[j].isp);
	// adding a simple opcode -> assembler conversion table would help
#if FRLOG_REGS
		fprintf(f, "d0 %08x d1 %08x d2 %08x d3 %08x\n", frlog[j].d[0], frlog[j].d[1], frlog[j].d[2], frlog[j].d[3]);
		fprintf(f, "d4 %08x d5 %08x d6 %08x d7 %08x\n", frlog[j].d[4], frlog[j].d[5], frlog[j].d[6], frlog[j].d[7]);
		fprintf(f, "a0 %08x a1 %08x a2 %08x a3 %08x\n", frlog[j].a[0], frlog[j].a[1], frlog[j].a[2], frlog[j].a[3]);
		fprintf(f, "a4 %08x a5 %08x a6 %08x a7 %08x\n", frlog[j].a[4], frlog[j].a[5], frlog[j].a[6], frlog[j].a[7]);
#endif
		m68k_disasm(f, frlog[j].pc, NULL, 1);
	}
	fclose(f);
}

void m68k_record_step(uaecptr pc, int opcode)
{
	static bool last_state = false;

#if FRLOG_HOTKEY
	if (! cpu_flight_recorder_active) {
		if (last_state) {
			// dump log out
		    	dump_flight_recorder();

			// remember last state
			last_state = false;
		}
		return;
	}
#endif

	if (! last_state) {
		// reset old log
		log_ptr = 0;
		memset(frlog, 0, sizeof(frlog));
		// remember last state
		last_state = true;
	}

#if FRLOG_REGS
	for (int i = 0; i < 8; i++) {
		frlog[log_ptr].d[i] = m68k_dreg(regs, i);
		frlog[log_ptr].a[i] = m68k_areg(regs, i);
	}
#endif
	frlog[log_ptr].pc = pc;

	MakeSR();
#if ! FRLOG_IRQ
	// is CPU in interrupt handler? Quit if should not be logged.
	if (regs.s && !regs.m) return;
#endif
	frlog[log_ptr].sr = regs.sr;
	frlog[log_ptr].usp = regs.usp;
	frlog[log_ptr].msp = regs.msp;
	frlog[log_ptr].isp = regs.isp;
	frlog[log_ptr].instr = opcode;

	log_ptr = (log_ptr + 1) % LOG_SIZE;
#if FRLOG_ALL
	if (log_ptr == 0) dump_flight_recorder();
#endif
}
#endif /* FLIGHT_RECORDER */

int broken_in;

static inline unsigned int cft_map (unsigned int f)
{
#if !defined(HAVE_GET_WORD_UNSWAPPED) || defined(FULLMMU)
    return f;
#else
    return do_byteswap_16(f);
#endif
}

void REGPARAM2 op_illg_1 (uae_u32 opcode)
{
    op_illg (cft_map (opcode));
}


void init_m68k (void)
{
    int i;

    for (i = 0 ; i < 256 ; i++) {
	int j;
	for (j = 0 ; j < 8 ; j++) {
		if (i & (1 << j)) break;
	}
	movem_index1[i] = j;
	movem_index2[i] = 7-j;
	movem_next[i] = i & (~(1 << j));
    }
    fpu_init (CPUType == 4);
}

void exit_m68k (void)
{
	fpu_exit ();
}

struct regstruct regs;
// MJ static struct regstruct regs_backup[16];
// MJ static int backup_pointer = 0;


#ifdef FULLMMU
static inline uae_u8 get_ibyte_1(uae_u32 o)
{
    return get_ibyte(o);
}
static inline uae_u16 get_iword_1(uae_u32 o)
{
    return get_iword(o);
}
static inline uae_u32 get_ilong_1(uae_u32 o)
{
    return get_ilong(o);
}
#else
# define get_ibyte_1(o) get_byte(m68k_getpc() + (o) + 1)
# define get_iword_1(o) get_word(m68k_getpc() + (o))
# define get_ilong_1(o) get_long(m68k_getpc() + (o))
#endif

/*
 * extract bitfield data from memory and return it in the MSBs
 * bdata caches the unmodified data for put_bitfield()
 */
uae_u32 get_bitfield(uae_u32 src, uae_u32 bdata[2], uae_s32 offset, int width)
{
	uae_u32 tmp, res, mask;

	offset &= 7;
	mask = 0xffffffffu << (32 - width);
	switch ((offset + width + 7) >> 3) {
	case 1:
		tmp = get_byte(src);
		res = tmp << (24 + offset);
		bdata[0] = tmp & ~(mask >> (24 + offset));
		break;
	case 2:
		tmp = get_word(src);
		res = tmp << (16 + offset);
		bdata[0] = tmp & ~(mask >> (16 + offset));
		break;
	case 3:
		tmp = get_word(src);
		res = tmp << (16 + offset);
		bdata[0] = tmp & ~(mask >> (16 + offset));
		tmp = get_byte(src + 2);
		res |= tmp << (8 + offset);
		bdata[1] = tmp & ~(mask >> (8 + offset));
		break;
	case 4:
		tmp = get_long(src);
		res = tmp << offset;
		bdata[0] = tmp & ~(mask >> offset);
		break;
	case 5:
		tmp = get_long(src);
		res = tmp << offset;
		bdata[0] = tmp & ~(mask >> offset);
		tmp = get_byte(src + 4);
		res |= tmp >> (8 - offset);
		bdata[1] = tmp & ~(mask << (8 - offset));
		break;
	default:
		/* Panic? */
		res = 0;
		break;
	}
	return res;
}

/*
 * write bitfield data (in the LSBs) back to memory, upper bits
 * must be cleared already.
 */
void put_bitfield(uae_u32 dst, uae_u32 bdata[2], uae_u32 val, uae_s32 offset, int width)
{
	offset = (offset & 7) + width;
	switch ((offset + 7) >> 3) {
	case 1:
		put_byte(dst, bdata[0] | (val << (8 - offset)));
		break;
	case 2:
		put_word(dst, bdata[0] | (val << (16 - offset)));
		break;
	case 3:
		put_word(dst, bdata[0] | (val >> (offset - 16)));
		put_byte(dst + 2, bdata[1] | (val << (24 - offset)));
		break;
	case 4:
		put_long(dst, bdata[0] | (val << (32 - offset)));
		break;
	case 5:
		put_long(dst, bdata[0] | (val >> (offset - 32)));
		put_byte(dst + 4, bdata[1] | (val << (40 - offset)));
		break;
	}
}

uae_u32 get_disp_ea_020 (uae_u32 base, uae_u32 dp)
{
    int reg = (dp >> 12) & 15;
    uae_s32 regd = regs.regs[reg];
    if ((dp & 0x800) == 0)
	regd = (uae_s32)(uae_s16)regd;
    regd <<= (dp >> 9) & 3;
    if (dp & 0x100) {
	uae_s32 outer = 0;
	if (dp & 0x80) base = 0;
	if (dp & 0x40) regd = 0;

	if ((dp & 0x30) == 0x20) base += (uae_s32)(uae_s16)next_iword();
	if ((dp & 0x30) == 0x30) base += next_ilong();

	if ((dp & 0x3) == 0x2) outer = (uae_s32)(uae_s16)next_iword();
	if ((dp & 0x3) == 0x3) outer = next_ilong();

	if ((dp & 0x4) == 0) base += regd;
	if (dp & 0x3) base = get_long (base);
	if (dp & 0x4) base += regd;

	return base + outer;
    } else {
	return base + (uae_s32)((uae_s8)dp) + regd;
    }
}

uae_u32 get_disp_ea_000 (uae_u32 base, uae_u32 dp)
{
    int reg = (dp >> 12) & 15;
    uae_s32 regd = regs.regs[reg];
#if 1
    if ((dp & 0x800) == 0)
	regd = (uae_s32)(uae_s16)regd;
    return base + (uae_s8)dp + regd;
#else
    /* Branch-free code... benchmark this again now that
     * things are no longer inline.  */
    uae_s32 regd16;
    uae_u32 mask;
    mask = ((dp & 0x800) >> 11) - 1;
    regd16 = (uae_s32)(uae_s16)regd;
    regd16 &= mask;
    mask = ~mask;
    base += (uae_s8)dp;
    regd &= mask;
    regd |= regd16;
    return base + regd;
#endif
}

void MakeSR (void)
{
#if 0
    assert((regs.t1 & 1) == regs.t1);
    assert((regs.t0 & 1) == regs.t0);
    assert((regs.s & 1) == regs.s);
    assert((regs.m & 1) == regs.m);
    assert((XFLG & 1) == XFLG);
    assert((NFLG & 1) == NFLG);
    assert((ZFLG & 1) == ZFLG);
    assert((VFLG & 1) == VFLG);
    assert((CFLG & 1) == CFLG);
#endif
    regs.sr = ((regs.t1 << 15) | (regs.t0 << 14)
	       | (regs.s << 13) | (regs.m << 12) | (regs.intmask << 8)
	       | (GET_XFLG() << 4) | (GET_NFLG() << 3) | (GET_ZFLG() << 2) | (GET_VFLG() << 1)
	       | GET_CFLG());
}

void MakeFromSR (void)
{
    int oldm = regs.m;
    int olds = regs.s;

    regs.t1 = (regs.sr >> 15) & 1;
    regs.t0 = (regs.sr >> 14) & 1;
    regs.s = (regs.sr >> 13) & 1;
    mmu_set_super(regs.s);
    regs.m = (regs.sr >> 12) & 1;
    regs.intmask = (regs.sr >> 8) & 7;
    SET_XFLG ((regs.sr >> 4) & 1);
    SET_NFLG ((regs.sr >> 3) & 1);
    SET_ZFLG ((regs.sr >> 2) & 1);
    SET_VFLG ((regs.sr >> 1) & 1);
    SET_CFLG (regs.sr & 1);
	if (olds != regs.s) {
	    if (olds) {
		if (oldm)
		    regs.msp = m68k_areg(regs, 7);
		else
		    regs.isp = m68k_areg(regs, 7);
		m68k_areg(regs, 7) = regs.usp;
	    } else {
		regs.usp = m68k_areg(regs, 7);
		m68k_areg(regs, 7) = regs.m ? regs.msp : regs.isp;
	    }
	} else if (olds && oldm != regs.m) {
	    if (oldm) {
		regs.msp = m68k_areg(regs, 7);
		m68k_areg(regs, 7) = regs.isp;
	    } else {
		regs.isp = m68k_areg(regs, 7);
		m68k_areg(regs, 7) = regs.msp;
	    }
	}

    // SPCFLAGS_SET( SPCFLAG_INT );
    SPCFLAGS_SET( SPCFLAG_INT );
    if (regs.t1 || regs.t0)
	SPCFLAGS_SET( SPCFLAG_TRACE );
    else
	SPCFLAGS_CLEAR( SPCFLAG_TRACE );
}

/* for building exception frames */
static inline void exc_push_word(uae_u16 w)
{
    m68k_areg(regs, 7) -= 2;
    put_word(m68k_areg(regs, 7), w);
}
static inline void exc_push_long(uae_u32 l)
{
    m68k_areg(regs, 7) -= 4;
    put_long (m68k_areg(regs, 7), l);
}

static inline void exc_make_frame(
		int format,
		uae_u16	sr,
		uae_u32 currpc,
		int nr,
		uae_u32 x0,
		uae_u32 x1
)
{
    switch(format) {
     case 4:
	exc_push_long(x1);
	exc_push_long(x0);
	break;
     case 3:
     case 2:
	exc_push_long(x0);
	break;
    }

    exc_push_word((format << 12) + (nr * 4));	/* format | vector */
    exc_push_long(currpc);
    exc_push_word(sr);
#if 0 /* debugging helpers; activate as needed */
	if (/* nr != 0x45  && */ /* Timer-C */
		nr != 0x1c && /* VBL */
		nr != 0x46)   /* ACIA */
	{
		memptr sp = m68k_areg(regs, 7);
		uae_u16 sr = get_word(sp);
		fprintf(stderr, "Exc:%02x  SP: %08x  USP: %08x  SR: %04x  PC: %08x  Format: %04x", nr, sp, regs.usp, sr, get_long(sp + 2), get_word(sp + 6));
		if (nr >= 32 && nr < 48)
		{
			fprintf(stderr, "  Opcode: $%04x", sr & 0x2000 ? get_word(sp + 8) : get_word(regs.usp));
		}
		fprintf(stderr, "\n");
	}
#endif
}


void ex_rte(void)
{
	uae_u16 newsr;
	uae_u32 newpc;
	uae_s16 format;

	for (;;)
	{
		newsr = get_word(m68k_areg(regs, 7));
		m68k_areg(regs, 7) += 2;
		newpc = get_long(m68k_areg(regs, 7));
		m68k_areg(regs, 7) += 4;
		format = get_word(m68k_areg(regs, 7));
		m68k_areg(regs, 7) += 2;
		if ((format & 0xF000) == 0x0000) break;
		else if ((format & 0xF000) == 0x1000) { ; }
		else if ((format & 0xF000) == 0x2000) { m68k_areg(regs, 7) += 4; break; }
//		else if ((format & 0xF000) == 0x3000) { m68k_areg(regs, 7) += 4; break; }
		else if ((format & 0xF000) == 0x7000) { m68k_areg(regs, 7) += 52; break; }
		else if ((format & 0xF000) == 0x8000) { m68k_areg(regs, 7) += 50; break; }
		else if ((format & 0xF000) == 0x9000) { m68k_areg(regs, 7) += 12; break; }
		else if ((format & 0xF000) == 0xa000) { m68k_areg(regs, 7) += 24; break; }
		else if ((format & 0xF000) == 0xb000) { m68k_areg(regs, 7) += 84; break; }
		else { Exception(14,0); return; }
		regs.sr = newsr;
		MakeFromSR();
	}
#if 0 /* debugging helpers; activate as needed */
	{
		memptr sp = m68k_areg(regs, 7) - 8;
		int nr = (format & 0xfff) >> 2;
		if (/* nr != 0x45 && */ /* Timer-C */
			nr != 0x1c && /* VBL */
			nr != 0x46)   /* ACIA */
			fprintf(stderr, "RTE     SP: %08x  USP: %08x  SR: %04x  PC: %08x  Format: %04x olds=%d nr=%02x -> %08x\n", sp, regs.usp, newsr, m68k_getpc(), format, regs.s, nr, newpc);
	}
#endif
	regs.sr = newsr;
	MakeFromSR();
	m68k_setpc_rte(newpc);
	fill_prefetch_0();
}

#ifdef EXCEPTIONS_VIA_LONGJMP
static int building_bus_fault_stack_frame=0;
#endif

void Exception(int nr, uaecptr oldpc)
{
    uae_u32 currpc = m68k_getpc ();
    MakeSR();

    if (fixup.flag)
    {
        m68k_areg(regs, fixup.reg) = fixup.value;
        fixup.flag = 0;
    }

    if (!regs.s) {
	regs.usp = m68k_areg(regs, 7);
	m68k_areg(regs, 7) = regs.m ? regs.msp : regs.isp;
	regs.s = 1;
	mmu_set_super(1);
    }

    if (nr == 2) {
    	/* BUS ERROR handler begins */
#ifdef ENABLE_EPSLIMITER
        check_eps_limit(currpc);
#endif
        // panicbug("Exception Nr. %d CPC: %08x NPC: %08x SP=%08x Addr: %08x", nr, currpc, get_long (regs.vbr + 4*nr), m68k_areg(regs, 7), regs.mmu_fault_addr);
#ifdef EXCEPTIONS_VIA_LONGJMP
	if (!building_bus_fault_stack_frame)
#else
	try
#endif
	{
#ifdef EXCEPTIONS_VIA_LONGJMP
            building_bus_fault_stack_frame= 1;
#endif
	    /* 68040 */
	    exc_push_long(0);	/* PD3 */
	    exc_push_long(0);	/* PD2 */
	    exc_push_long(0);	/* PD1 */
	    exc_push_long(0);	/* PD0/WB1D */
	    exc_push_long(0);	/* WB1A */
	    exc_push_long(0);	/* WB2D */
	    exc_push_long(0);	/* WB2A */
	    exc_push_long(regs.wb3_data);	/* WB3D */
	    exc_push_long(regs.mmu_fault_addr);	/* WB3A */
	    exc_push_long(regs.mmu_fault_addr);
	    exc_push_word(0);	/* WB1S */
	    exc_push_word(0);	/* WB2S */
	    exc_push_word(regs.wb3_status);	/* WB3S */
	    regs.wb3_status = 0;
	    exc_push_word(regs.mmu_ssw);
	    exc_push_long(regs.mmu_fault_addr); /* EA */
	    exc_make_frame(7, regs.sr, regs.fault_pc, 2, 0, 0);

	}
#ifdef EXCEPTIONS_VIA_LONGJMP
	else
#else
	catch (m68k_exception)
#endif
	{
            report_double_bus_error();
#ifdef EXCEPTIONS_VIA_LONGJMP
            building_bus_fault_stack_frame= 0;
#endif
	    return;
        }

#ifdef EXCEPTIONS_VIA_LONGJMP
	building_bus_fault_stack_frame= 0;
#endif
        /* end of BUS ERROR handler */
    } else if (nr == 3) {
	exc_make_frame(2, regs.sr, last_addr_for_exception_3, nr,
			last_fault_for_exception_3 & 0xfffffffe, 0);
    } else if (nr == 5 || nr == 6 || nr == 7 || nr == 9) {
	/* div by zero, CHK, TRAP or TRACE */
	exc_make_frame(2, regs.sr, currpc, nr, oldpc, 0);
    } else if (regs.m && nr >= 24 && nr < 32) {
	/* interrupts! */
	exc_make_frame(0, regs.sr, currpc, nr, 0, 0);
	regs.sr |= (1 << 13);
	regs.msp = m68k_areg(regs, 7);
	m68k_areg(regs, 7) = regs.isp;

	exc_make_frame(1,	/* throwaway */
			regs.sr, currpc, nr, 0, 0);
    } else {
	exc_make_frame(0, regs.sr, currpc, nr, 0, 0);
    }
    m68k_setpc (get_long (regs.vbr + 4*nr));
#ifdef USE_JIT
    if (UseJIT)
        SPCFLAGS_SET( SPCFLAG_JIT_END_COMPILE );
#endif
    fill_prefetch_0 ();
    regs.t1 = regs.t0 = regs.m = 0;
    SPCFLAGS_CLEAR(SPCFLAG_TRACE | SPCFLAG_DOTRACE);
}

static void Interrupt(int nr)
{
    if (UseDeferredInterruptModel() && trace_irqmanaged_env()) {
        static unsigned long trace_pre_count = 0;
        if (trace_pre_count < 4000) {
            MakeSR();
            fprintf(stderr, "IRQM interrupt pre %lu pc=%08x nr=%d sr=%04x intmask=%u spc=%08x live=%08x\n",
                ++trace_pre_count,
                (unsigned)m68k_getpc(), nr, (unsigned)regs.sr,
                (unsigned)regs.intmask, (unsigned)regs.spcflags,
                (unsigned)InterruptFlags);
        }
    }
    assert(nr < 8 && nr >= 0);
    Exception(nr+24, 0);

    regs.intmask = nr;
    // why the hell the SPCFLAG_INT is to be set??? (joy)
    // regs.spcflags |= SPCFLAG_INT; (disabled by joy)
    SPCFLAGS_SET( SPCFLAG_INT );
    if (UseDeferredInterruptModel() && trace_irqmanaged_env()) {
        static unsigned long trace_post_count = 0;
        if (trace_post_count < 4000) {
            MakeSR();
            fprintf(stderr, "IRQM interrupt post %lu pc=%08x nr=%d sr=%04x intmask=%u spc=%08x live=%08x\n",
                ++trace_post_count,
                (unsigned)m68k_getpc(), nr, (unsigned)regs.sr,
                (unsigned)regs.intmask, (unsigned)regs.spcflags,
                (unsigned)InterruptFlags);
        }
    }
}

#if 0
static void SCCInterrupt(int nr)
{
    // fprintf(stderr, "CPU: in SCCInterrupt\n");
    Exception(nr, 0);

    regs.intmask = 5;// ex 5
}

static void MFPInterrupt(int nr)
{
    // fprintf(stderr, "CPU: in MFPInterrupt\n");
    Exception(nr, 0);

    regs.intmask = 6;
}
#endif

int m68k_move2c (int regno, uae_u32 *regp)
{
	switch (regno) {
	 case 0: regs.sfc = *regp & 7; break;
	 case 1: regs.dfc = *regp & 7; break;
	 case 2: regs.cacr = *regp & 0x80008000;
#ifdef USE_JIT
		 set_cache_state(regs.cacr & 0x8000);
		 if (*regp & 0x08) {	/* Just to be on the safe side */
			flush_icache();
		 }
#endif
		 break;
	 case 3: mmu_set_tc(*regp & 0xc000); break;
	 case 4:
	 case 5:
	 case 6:
	 case 7: mmu_set_ttr(regno, *regp & 0xffffe364); break;
	 case 0x800: regs.usp = *regp; break;
	 case 0x801: regs.vbr = *regp; break;
	 case 0x802: regs.caar = *regp & 0xfc; break;
	 case 0x803: regs.msp = *regp; if (regs.m == 1) m68k_areg(regs, 7) = regs.msp; break;
	 case 0x804: regs.isp = *regp; if (regs.m == 0) m68k_areg(regs, 7) = regs.isp; break;
	 case 0x805: mmu_set_mmusr(*regp); break;
	 case 0x806: regs.urp = *regp & MMU_ROOT_PTR_ADDR_MASK; break;
	 case 0x807: regs.srp = *regp & MMU_ROOT_PTR_ADDR_MASK; break;
	 default:
	    op_illg (0x4E7B);
	    return 0;
	}
    return 1;
}

int m68k_movec2 (int regno, uae_u32 *regp)
{
	switch (regno) {
	 case 0: *regp = regs.sfc; break;
	 case 1: *regp = regs.dfc; break;
	 case 2: *regp = regs.cacr; break;
	 case 3: *regp = regs.tc; break;
	 case 4: *regp = regs.itt0; break;
	 case 5: *regp = regs.itt1; break;
	 case 6: *regp = regs.dtt0; break;
	 case 7: *regp = regs.dtt1; break;
	 case 0x800: *regp = regs.usp; break;
	 case 0x801: *regp = regs.vbr; break;
	 case 0x802: *regp = regs.caar; break;
	 case 0x803: *regp = regs.m == 1 ? m68k_areg(regs, 7) : regs.msp; break;
	 case 0x804: *regp = regs.m == 0 ? m68k_areg(regs, 7) : regs.isp; break;
	 case 0x805: *regp = regs.mmusr; break;
	 case 0x806: *regp = regs.urp; break;
	 case 0x807: *regp = regs.srp; break;
	 default:
	    op_illg (0x4E7A);
	    return 0;
	}
    return 1;
}

#if !defined(uae_s64)
static inline int
div_unsigned(uae_u32 src_hi, uae_u32 src_lo, uae_u32 div, uae_u32 *quot, uae_u32 *rem)
{
	uae_u32 q = 0, cbit = 0;
	int i;

	if (div <= src_hi) {
	    return 1;
	}
	for (i = 0 ; i < 32 ; i++) {
		cbit = src_hi & 0x80000000ul;
		src_hi <<= 1;
		if (src_lo & 0x80000000ul) src_hi++;
		src_lo <<= 1;
		q = q << 1;
		if (cbit || div <= src_hi) {
			q |= 1;
			src_hi -= div;
		}
	}
	*quot = q;
	*rem = src_hi;
	return 0;
}
#endif

void m68k_divl (uae_u32 /*opcode*/, uae_u32 src, uae_u16 extra, uaecptr oldpc)
{
#if defined(uae_s64)
    if (src == 0) {
	Exception (5, oldpc);
	return;
    }
    if (extra & 0x800) {
	/* signed variant */
	uae_s64 a = (uae_s64)(uae_s32)m68k_dreg(regs, (extra >> 12) & 7);
	uae_s64 quot, rem;

	if (extra & 0x400) {
	    a &= 0xffffffffu;
	    a |= (uae_s64)m68k_dreg(regs, extra & 7) << 32;
	}
	rem = a % (uae_s64)(uae_s32)src;
	quot = a / (uae_s64)(uae_s32)src;
	if ((quot & UVAL64(0xffffffff80000000)) != 0
	    && (quot & UVAL64(0xffffffff80000000)) != UVAL64(0xffffffff80000000))
	{
	    SET_VFLG (1);
	    SET_NFLG (1);
	    SET_CFLG (0);
	} else {
	    if (((uae_s32)rem < 0) != ((uae_s64)a < 0)) rem = -rem;
	    SET_VFLG (0);
	    SET_CFLG (0);
	    SET_ZFLG (((uae_s32)quot) == 0);
	    SET_NFLG (((uae_s32)quot) < 0);
	    m68k_dreg(regs, extra & 7) = rem;
	    m68k_dreg(regs, (extra >> 12) & 7) = quot;
	}
    } else {
	/* unsigned */
	uae_u64 a = (uae_u64)(uae_u32)m68k_dreg(regs, (extra >> 12) & 7);
	uae_u64 quot, rem;

	if (extra & 0x400) {
	    a &= 0xffffffffu;
	    a |= (uae_u64)m68k_dreg(regs, extra & 7) << 32;
	}
	rem = a % (uae_u64)src;
	quot = a / (uae_u64)src;
	if (quot > 0xffffffffu) {
	    SET_VFLG (1);
	    SET_NFLG (1);
	    SET_CFLG (0);
	} else {
	    SET_VFLG (0);
	    SET_CFLG (0);
	    SET_ZFLG (((uae_s32)quot) == 0);
	    SET_NFLG (((uae_s32)quot) < 0);
	    m68k_dreg(regs, extra & 7) = rem;
	    m68k_dreg(regs, (extra >> 12) & 7) = quot;
	}
    }
#else
    if (src == 0) {
	Exception (5, oldpc);
	return;
    }
    if (extra & 0x800) {
	/* signed variant */
	uae_s32 lo = (uae_s32)m68k_dreg(regs, (extra >> 12) & 7);
	uae_s32 hi = lo < 0 ? -1 : 0;
	uae_s32 save_high;
	uae_u32 quot, rem;
	uae_u32 sign;

	if (extra & 0x400) {
	    hi = (uae_s32)m68k_dreg(regs, extra & 7);
	}
	save_high = hi;
	sign = (hi ^ src);
	if (hi < 0) {
	    hi = ~hi;
	    lo = -lo;
	    if (lo == 0) hi++;
	}
	if ((uae_s32)src < 0) src = -src;
	if (div_unsigned(hi, lo, src, &quot, &rem) ||
	    (sign & 0x80000000) ? quot > 0x80000000 : quot > 0x7fffffff) {
	    SET_VFLG (1);
	    SET_NFLG (1);
	    SET_CFLG (0);
	} else {
	    if (sign & 0x80000000) quot = -quot;
	    if (((uae_s32)rem < 0) != (save_high < 0)) rem = -rem;
	    SET_VFLG (0);
	    SET_CFLG (0);
	    SET_ZFLG (((uae_s32)quot) == 0);
	    SET_NFLG (((uae_s32)quot) < 0);
	    m68k_dreg(regs, extra & 7) = rem;
	    m68k_dreg(regs, (extra >> 12) & 7) = quot;
	}
    } else {
	/* unsigned */
	uae_u32 lo = (uae_u32)m68k_dreg(regs, (extra >> 12) & 7);
	uae_u32 hi = 0;
	uae_u32 quot, rem;

	if (extra & 0x400) {
	    hi = (uae_u32)m68k_dreg(regs, extra & 7);
	}
	if (div_unsigned(hi, lo, src, &quot, &rem)) {
	    SET_VFLG (1);
	    SET_NFLG (1);
	    SET_CFLG (0);
	} else {
	    SET_VFLG (0);
	    SET_CFLG (0);
	    SET_ZFLG (((uae_s32)quot) == 0);
	    SET_NFLG (((uae_s32)quot) < 0);
	    m68k_dreg(regs, extra & 7) = rem;
	    m68k_dreg(regs, (extra >> 12) & 7) = quot;
	}
    }
#endif
}

#if !defined(uae_s64)
static inline void
mul_unsigned(uae_u32 src1, uae_u32 src2, uae_u32 *dst_hi, uae_u32 *dst_lo)
{
	uae_u32 r0 = (src1 & 0xffff) * (src2 & 0xffff);
	uae_u32 r1 = ((src1 >> 16) & 0xffff) * (src2 & 0xffff);
	uae_u32 r2 = (src1 & 0xffff) * ((src2 >> 16) & 0xffff);
	uae_u32 r3 = ((src1 >> 16) & 0xffff) * ((src2 >> 16) & 0xffff);
	uae_u32 lo;

	lo = r0 + ((r1 << 16) & 0xffff0000ul);
	if (lo < r0) r3++;
	r0 = lo;
	lo = r0 + ((r2 << 16) & 0xffff0000ul);
	if (lo < r0) r3++;
	r3 += ((r1 >> 16) & 0xffff) + ((r2 >> 16) & 0xffff);
	*dst_lo = lo;
	*dst_hi = r3;
}
#endif

void m68k_mull (uae_u32 /*opcode*/, uae_u32 src, uae_u16 extra)
{
#if defined(uae_s64)
    if (extra & 0x800) {
	/* signed variant */
	uae_s64 a = (uae_s64)(uae_s32)m68k_dreg(regs, (extra >> 12) & 7);

	a *= (uae_s64)(uae_s32)src;
	SET_VFLG (0);
	SET_CFLG (0);
	SET_ZFLG (a == 0);
	SET_NFLG (a < 0);
	if (extra & 0x400)
	    m68k_dreg(regs, extra & 7) = a >> 32;
	else if ((a & UVAL64(0xffffffff80000000)) != 0
		 && (a & UVAL64(0xffffffff80000000)) != UVAL64(0xffffffff80000000))
	{
	    SET_VFLG (1);
	}
	m68k_dreg(regs, (extra >> 12) & 7) = (uae_u32)a;
    } else {
	/* unsigned */
	uae_u64 a = (uae_u64)(uae_u32)m68k_dreg(regs, (extra >> 12) & 7);

	a *= (uae_u64)src;
	SET_VFLG (0);
	SET_CFLG (0);
	SET_ZFLG (a == 0);
	SET_NFLG (((uae_s64)a) < 0);
	if (extra & 0x400)
	    m68k_dreg(regs, extra & 7) = a >> 32;
	else if ((a & UVAL64(0xffffffff00000000)) != 0) {
	    SET_VFLG (1);
	}
	m68k_dreg(regs, (extra >> 12) & 7) = (uae_u32)a;
    }
#else
    if (extra & 0x800) {
	/* signed variant */
	uae_s32 src1,src2;
	uae_u32 dst_lo,dst_hi;
	uae_u32 sign;

	src1 = (uae_s32)src;
	src2 = (uae_s32)m68k_dreg(regs, (extra >> 12) & 7);
	sign = (src1 ^ src2);
	if (src1 < 0) src1 = -src1;
	if (src2 < 0) src2 = -src2;
	mul_unsigned((uae_u32)src1,(uae_u32)src2,&dst_hi,&dst_lo);
	if (sign & 0x80000000) {
		dst_hi = ~dst_hi;
		dst_lo = -dst_lo;
		if (dst_lo == 0) dst_hi++;
	}
	SET_VFLG (0);
	SET_CFLG (0);
	SET_ZFLG (dst_hi == 0 && dst_lo == 0);
	SET_NFLG (((uae_s32)dst_hi) < 0);
	if (extra & 0x400)
	    m68k_dreg(regs, extra & 7) = dst_hi;
	else if ((dst_hi != 0 || (dst_lo & 0x80000000) != 0)
		 && ((dst_hi & 0xffffffff) != 0xffffffff
		     || (dst_lo & 0x80000000) != 0x80000000))
	{
	    SET_VFLG (1);
	}
	m68k_dreg(regs, (extra >> 12) & 7) = dst_lo;
    } else {
	/* unsigned */
	uae_u32 dst_lo,dst_hi;

	mul_unsigned(src,(uae_u32)m68k_dreg(regs, (extra >> 12) & 7),&dst_hi,&dst_lo);

	SET_VFLG (0);
	SET_CFLG (0);
	SET_ZFLG (dst_hi == 0 && dst_lo == 0);
	SET_NFLG (((uae_s32)dst_hi) < 0);
	if (extra & 0x400)
	    m68k_dreg(regs, extra & 7) = dst_hi;
	else if (dst_hi != 0) {
	    SET_VFLG (1);
	}
	m68k_dreg(regs, (extra >> 12) & 7) = dst_lo;
    }
#endif
}

// If value is greater than zero, this means we are still processing an EmulOp
// because the counter is incremented only in m68k_execute(), i.e. interpretive
// execution only
#ifdef USE_JIT
static int m68k_execute_depth = 0;
#endif

void m68k_reset (void)
{
    regs.s = 1;
    regs.m = 0;
    regs.stopped = 0;
    regs.t1 = 0;
    regs.t0 = 0;
    SET_ZFLG (0);
    SET_XFLG (0);
    SET_CFLG (0);
    SET_VFLG (0);
    SET_NFLG (0);
    SPCFLAGS_INIT( 0 );
    regs.intmask = 7;
    regs.vbr = regs.sfc = regs.dfc = 0;

    // need to ensure the following order of initialization is correct
    // (it is definitely better than what it was before this commit
    //  since it was reading from 0x00000000 in User mode and with active MMU)
    mmu_set_tc(regs.tc & ~0x8000); /* disable mmu */
#if 0
    m68k_areg (regs, 7) = phys_get_long(0x00000000);
#else
    m68k_areg (regs, 7) = 0x2000;
#endif
#if 0
    m68k_setpc (phys_get_long(0x00000004));
#else
    m68k_setpc (ROMBaseMac + 0x2a);
#endif
    fill_prefetch_0 ();

    /* gb-- moved into {fpp,fpu_x86}.cpp::fpu_init()
    regs.fpcr = regs.fpsr = regs.fpiar = 0; */
    fpu_reset();
#if 0
    // MMU
    mmu_reset();
    mmu_set_super(1);
    // Cache
    regs.cacr = 0;
    regs.caar = 0;
#endif
#ifdef FLIGHT_RECORDER
	log_ptr = 0;
	memset(frlog, 0, sizeof(frlog));
#endif
}

void m68k_emulop_return(void)
{
	SPCFLAGS_SET( SPCFLAG_BRK );
	quit_program = 1;
}

#if 0
static void save_regs(struct M68kRegisters &r)
{
	int i;
	
	for (i=0; i<8; i++) {
		r.d[i] = m68k_dreg(regs, i);
		r.a[i] = m68k_areg(regs, i);
	}
	r.pc = m68k_getpc();
	MakeSR();
	r.sr = regs.sr;
	r.isp = regs.isp;
	r.usp = regs.usp;
	r.msp = regs.msp;
	if ((r.sr & 0x2000) == 0)
		r.usp = r.a[7];
	else if ((r.sr & 0x1000) != 0)
		r.msp = r.a[7];
	else
		r.isp = r.a[7];
}
#endif

#if 0
static void restore_regs(struct M68kRegisters &r)
{
	int i;
	
	for (i=0; i<8; i++) {
		m68k_dreg(regs, i) = r.d[i];
		m68k_areg(regs, i) = r.a[i];
	}
	regs.isp = r.isp;
	regs.usp = r.usp;
	regs.msp = r.msp;
	regs.sr = r.sr;
	MakeFromSR();
}
#endif

void m68k_emulop(uae_u32 opcode)
{
#if 0
	struct M68kRegisters r = {};
	save_regs(r);
	if (EmulOp(opcode, &r))
		restore_regs(r);
#else
	struct M68kRegisters r = {};
	int i;
	uae_u32 old_pc = m68k_getpc();

	for (i=0; i<8; i++) {
		r.d[i] = m68k_dreg(regs, i);
		r.a[i] = m68k_areg(regs, i);
	}
	MakeSR();
	r.sr = regs.sr;
	if (trace_d6_enabled())
		fprintf(stderr, "TRACE_D6 m68k_emulop enter opcode=%04x pc=%08x d6=%08x d7=%08x a4=%08x a5=%08x\n", (unsigned)opcode, m68k_getpc(), r.d[6], r.d[7], r.a[4], r.a[5]);
	if (trace_emulopflow_env() && trace_emulopflow_opcode(opcode) && trace_emulopflow_count < trace_emulopflow_limit()) {
		fprintf(stderr,
			"EMUFLOW %lu M68K_EMULOP_ENTER op=%04x pc=%08x sr=%04x spc=%08x d0=%08x d1=%08x d2=%08x a0=%08x a1=%08x a7=%08x\n",
			++trace_emulopflow_count,
			(unsigned)opcode,
			(unsigned)old_pc,
			(unsigned)r.sr,
			(unsigned)regs.spcflags,
			(unsigned)r.d[0],
			(unsigned)r.d[1],
			(unsigned)r.d[2],
			(unsigned)r.a[0],
			(unsigned)r.a[1],
			(unsigned)r.a[7]);
	}
	EmulOp(opcode, &r);
	if (trace_d6_enabled())
		fprintf(stderr, "TRACE_D6 m68k_emulop leave opcode=%04x pc=%08x d6=%08x d7=%08x a4=%08x a5=%08x\n", (unsigned)opcode, m68k_getpc(), r.d[6], r.d[7], r.a[4], r.a[5]);
	if (trace_emulopflow_env() && trace_emulopflow_opcode(opcode) && trace_emulopflow_count < trace_emulopflow_limit()) {
		fprintf(stderr,
			"EMUFLOW %lu M68K_EMULOP_LEAVE op=%04x oldpc=%08x hostpc=%08x sr=%04x spc=%08x quit=%d d0=%08x d1=%08x d2=%08x a0=%08x a1=%08x a7=%08x\n",
			++trace_emulopflow_count,
			(unsigned)opcode,
			(unsigned)old_pc,
			(unsigned)m68k_getpc(),
			(unsigned)r.sr,
			(unsigned)regs.spcflags,
			quit_program,
			(unsigned)r.d[0],
			(unsigned)r.d[1],
			(unsigned)r.d[2],
			(unsigned)r.a[0],
			(unsigned)r.a[1],
			(unsigned)r.a[7]);
	}
	for (i=0; i<8; i++) {
		m68k_dreg(regs, i) = r.d[i];
		m68k_areg(regs, i) = r.a[i];
	}
	regs.sr = r.sr;
	MakeFromSR();
#endif
}

#if 0
void m68k_natfeat_id(void)
{
	struct M68kRegisters r = {};

	/* is it really necessary to save all registers? */
	save_regs(r);

	memptr stack = r.a[7] + 4;	/* skip return address */
	r.d[0] = nf_get_id(stack);

	restore_regs(r);
}

void m68k_natfeat_call(void)
{
	struct M68kRegisters r = {};

	/* is it really necessary to save all registers? */
	save_regs(r);

	memptr stack = r.a[7] + 4;	/* skip return address */
	bool isSupervisorMode = ((r.sr & 0x2000) == 0x2000);
	r.d[0] = nf_call(stack, isSupervisorMode);

	restore_regs(r);
}
#endif

#if 0
static int m68k_call(uae_u32 pc)
{
	VOLATILE int exc = 0;
	m68k_setpc(pc);
    TRY(prb) {
#ifdef USE_JIT
		if (UseJIT) {
			exec_nostats();
			//			m68k_do_compile_execute();
			// The above call to m68k_do_compile_execute fails with BadAccess in sigsegv_handler (MAC, if it is executed after the first compile_block)
			// (NULL pointer to addr_instr).
			// Call exec_nostats avoids calling compile_block, because stack modification is only temporary
			// which will fill up compile cache with BOGUS data.
			// we can call exec_nostats directly, do our code, and return back here.
		}
		else
#endif
			m68k_do_execute();
    }
    CATCH(prb) {
    	exc = int(prb);
    }
    return exc;
}

static uae_u32 m68k_alloca(int size)
{
	uae_u32 sp = (m68k_areg(regs, 7) - size) & ~1;
	m68k_areg(regs, 7) = sp;
	if ((regs.sr & 0x2000) == 0)
		regs.usp = sp;
	else if ((regs.sr & 0x1000) != 0)
		regs.msp = sp;
	else
		regs.isp = sp;
	return sp;
}
#endif

#if 0
uae_u32 linea68000(volatile uae_u16 opcode)
{
	sigjmp_buf jmp;
	struct M68kRegisters r = {};
	volatile uae_u32 abase = 0;
	
	SAVE_EXCEPTION;
	save_regs(r);

	const int sz = 8 + sizeof(void *);
	volatile uae_u32 sp = 0;
	uae_u32 backup[(sz + 3) / 4];

	if (sigsetjmp(jmp, 1) == 0)
	{
		void *p = jmp;
		uae_u8 *sp_p;
		int exc;
		
		sp = m68k_alloca(sz);
		memcpy(backup, phys_get_real_address(sp), sz);

		WriteHWMemInt16(sp, opcode);
		WriteHWMemInt16(sp + 2, 0xa0ff);
		WriteHWMemInt32(sp + 4, 13);
		sp_p = phys_get_real_address(sp + 8);
		*((void **)sp_p) = p;
		if ((exc = m68k_call(sp)) != 0)
		{
			panicbug("exception %d in LINEA", exc);
			m68k_dreg(regs, 0) = 0;
		}
	} else
	{
		abase = m68k_dreg(regs, 0);
	}

	if (sp)	{
		memcpy(phys_get_real_address(sp), backup, sz);
	}
	restore_regs(r);
	m68k_setpc(r.pc);
    RESTORE_EXCEPTION;
	return abase;
}
#endif

#if 0
static void rts68000()
{
	uae_u32 SP = m68k_getpc() + 6;
	sigjmp_buf *p;
	uae_u8 *sp_p = phys_get_real_address(SP);
	
	p = (sigjmp_buf *)(*((void **)sp_p));
	SP += sizeof(void *);
	m68k_areg(regs, 7) = SP;
	siglongjmp(*p, 1);
}
#endif

void REGPARAM2 op_illg (uae_u32 opcode)
{
	if ((opcode & 0xF000) == 0xA000) {
#if 0
		if (opcode == 0xa0ff)
		{
			uaecptr pc = m68k_getpc();
			uae_u32 call = ReadHWMemInt32(pc + 2);
			switch (call)
			{
			case 13:
				rts68000();
				return;
			}
			m68k_setpc(pc + 6);
		}
#endif
		Exception(0xA,0);
		return;
	}

	if ((opcode & 0xF000) == 0xF000) {
		Exception(0xB,0);
		return;
	}

	D(bug("Illegal instruction: %04x at %08x", opcode, m68k_getpc()));
#if defined(USE_JIT) && defined(JIT_DEBUG)
	compiler_dumpstate();
#endif

	Exception (4,0);
	return;
}

static uaecptr last_trace_ad = 0;

static void do_trace (void)
{
    if (regs.t0) {
       uae_u16 opcode;
       /* should also include TRAP, CHK, SR modification FPcc */
       /* probably never used so why bother */
       /* We can afford this to be inefficient... */
       m68k_setpc (m68k_getpc ());
       fill_prefetch_0 ();
       opcode = get_word(m68k_getpc());
       if (opcode == 0x4e72            /* RTE */
           || opcode == 0x4e74                 /* RTD */
           || opcode == 0x4e75                 /* RTS */
           || opcode == 0x4e77                 /* RTR */
           || opcode == 0x4e76                 /* TRAPV */
           || (opcode & 0xffc0) == 0x4e80      /* JSR */
           || (opcode & 0xffc0) == 0x4ec0      /* JMP */
           || (opcode & 0xff00) == 0x6100  /* BSR */
           || ((opcode & 0xf000) == 0x6000     /* Bcc */
               && cctrue((opcode >> 8) & 0xf))
           || ((opcode & 0xf0f0) == 0x5050 /* DBcc */
               && !cctrue((opcode >> 8) & 0xf)
               && (uae_s16)m68k_dreg(regs, opcode & 7) != 0))
      {
 	    last_trace_ad = m68k_getpc ();
	    SPCFLAGS_CLEAR( SPCFLAG_TRACE );
	    SPCFLAGS_SET( SPCFLAG_DOTRACE );
	}
    } else if (regs.t1) {
       last_trace_ad = m68k_getpc ();
       SPCFLAGS_CLEAR( SPCFLAG_TRACE );
       SPCFLAGS_SET( SPCFLAG_DOTRACE );
    }
}

#if 0
#define SERVE_VBL_MFP(resetStop)							\
{															\
	if (SPCFLAGS_TEST( SPCFLAG_INT3|SPCFLAG_VBL|SPCFLAG_INT5|SPCFLAG_SCC|SPCFLAG_MFP )) {		\
		if (SPCFLAGS_TEST( SPCFLAG_INT3 )) {					\
			if (3 > regs.intmask) {							\
				Interrupt(3);								\
				regs.stopped = 0;							\
				SPCFLAGS_CLEAR( SPCFLAG_INT3 );				\
				if (resetStop)								\
					SPCFLAGS_CLEAR( SPCFLAG_STOP );			\
			}												\
		}													\
		if (SPCFLAGS_TEST( SPCFLAG_VBL )) {					\
			if (4 > regs.intmask) {							\
				Interrupt(4);								\
				regs.stopped = 0;							\
				SPCFLAGS_CLEAR( SPCFLAG_VBL );				\
				if (resetStop)								\
					SPCFLAGS_CLEAR( SPCFLAG_STOP );			\
			}												\
		}													\
		if (SPCFLAGS_TEST( SPCFLAG_INT5 )) {					\
			if (5 > regs.intmask) {							\
				Interrupt(5);								\
				regs.stopped = 0;							\
				SPCFLAGS_CLEAR( SPCFLAG_INT5 );				\
				if (resetStop)								\
					SPCFLAGS_CLEAR( SPCFLAG_STOP );			\
			}												\
		}													\
		if (SPCFLAGS_TEST( SPCFLAG_SCC )) {					\
			if (5 > regs.intmask) {						\
				int vector_number=SCCdoInterrupt();			\
				if(vector_number){					\
					 SCCInterrupt(vector_number);	        	\
					regs.stopped = 0;				\
					SPCFLAGS_CLEAR( SPCFLAG_SCC);		\
					if (resetStop)					\
						SPCFLAGS_CLEAR( SPCFLAG_STOP );		\
				}							\
				else							\
					SPCFLAGS_CLEAR( SPCFLAG_SCC );		\
			}								\
		}									\
		if (SPCFLAGS_TEST( SPCFLAG_MFP )) {					\
			if (6 > regs.intmask) {							\
				int vector_number = MFPdoInterrupt();		\
				if (vector_number) {						\
					MFPInterrupt(vector_number);			\
					regs.stopped = 0;						\
					if (resetStop)							\
						SPCFLAGS_CLEAR( SPCFLAG_STOP );		\
				}											\
				else										\
					SPCFLAGS_CLEAR( SPCFLAG_MFP );			\
			}												\
		}													\
	}														\
}

#define SERVE_INTERNAL_IRQ()								\
{															\
	if (SPCFLAGS_TEST( SPCFLAG_INTERNAL_IRQ )) {			\
		SPCFLAGS_CLEAR( SPCFLAG_INTERNAL_IRQ );				\
		invoke200HzInterrupt();								\
	}														\
}
#endif

int m68k_do_specialties(void)
{
#if 0
	SERVE_INTERNAL_IRQ();
#endif
#ifdef USE_JIT
	if (UseJIT) {
		// Block was compiled
		SPCFLAGS_CLEAR( SPCFLAG_JIT_END_COMPILE );

		// Retain the request to get out of compiled code until
		// we reached the toplevel execution, i.e. the one that
		// can compile then run compiled code. This also means
		// we processed all (nested) EmulOps
		if ((m68k_execute_depth == 0) && SPCFLAGS_TEST( SPCFLAG_JIT_EXEC_RETURN ))
			SPCFLAGS_CLEAR( SPCFLAG_JIT_EXEC_RETURN );
	}
#endif
	/*n_spcinsns++;*/
	if (SPCFLAGS_TEST( SPCFLAG_DOTRACE )) {
		Exception (9,last_trace_ad);
	}
#if 0 /* not for ARAnyM; emulating 040 only */
	if ((regs.spcflags & SPCFLAG_STOP) && regs.s == 0 && currprefs.cpu_model <= 68010) {
		// 68000/68010 undocumented special case:
		// if STOP clears S-bit and T was not set:
		// cause privilege violation exception, PC pointing to following instruction.
		// If T was set before STOP: STOP works as documented.
		m68k_unset_stop();
		Exception(8, 0);
	}
#endif
	while (SPCFLAGS_TEST( SPCFLAG_STOP )) {
		//TODO: Check
#if 0
		if ((regs.sr & 0x700) == 0x700)
		{
			panicbug("STOPed with interrupts disabled, exiting; pc=$%08x", m68k_getpc());
			m68k_dumpstate (stderr, NULL);
			quit_program = 1;
#ifdef FULL_HISTORY
			ndebug::showHistory(20, false);
			m68k_dumpstate (stderr, NULL);
#endif
			return 1;
		}
#endif
		// give unused time slices back to OS when STOP instruction is executing
		SleepAndWait();
		if (SPCFLAGS_TEST( SPCFLAG_INT | SPCFLAG_DOINT )){
			if (UseDeferredInterruptModel() && trace_irqmanaged_env()) {
				static unsigned long stop_pre_count = 0;
				if (stop_pre_count < 4000) {
					MakeSR();
					fprintf(stderr, "IRQM spec-stop pre %lu pc=%08x sr=%04x intmask=%u spc=%08x live=%08x\n",
						++stop_pre_count,
						(unsigned)m68k_getpc(), (unsigned)regs.sr,
						(unsigned)regs.intmask, (unsigned)regs.spcflags,
						(unsigned)InterruptFlags);
				}
			}
			SPCFLAGS_CLEAR( SPCFLAG_INT | SPCFLAG_DOINT );
			int intr = intlev ();
			if (UseDeferredInterruptModel() && trace_irqmanaged_env()) {
				static unsigned long stop_post_count = 0;
				if (stop_post_count < 4000) {
					MakeSR();
					fprintf(stderr, "IRQM spec-stop post %lu pc=%08x sr=%04x intmask=%u spc=%08x intr=%d live=%08x\n",
						++stop_post_count,
						(unsigned)m68k_getpc(), (unsigned)regs.sr,
						(unsigned)regs.intmask, (unsigned)regs.spcflags,
						intr, (unsigned)InterruptFlags);
				}
			}
			if (intr != -1 && intr > regs.intmask) {
				Interrupt (intr);
				if (UseDeferredInterruptModel())
					SPCFLAGS_CLEAR( SPCFLAG_INT | SPCFLAG_DOINT );
				regs.stopped = 0;
				SPCFLAGS_CLEAR( SPCFLAG_STOP );
			}
		}

#if 0
		SERVE_INTERNAL_IRQ();
		SERVE_VBL_MFP(true);
#endif
#if 0
		if (SPCFLAGS_TEST( SPCFLAG_BRK ))
			break;
#endif
	}
	if (SPCFLAGS_TEST( SPCFLAG_TRACE ))
		do_trace ();

#if 0
	SERVE_VBL_MFP(false);
#endif

/*
// do not understand the INT vs DOINT stuff so I disabled it (joy)
	if (regs.spcflags & SPCFLAG_INT) {
		regs.spcflags &= ~SPCFLAG_INT;
		regs.spcflags |= SPCFLAG_DOINT;
	}
*/
	if (SPCFLAGS_TEST( SPCFLAG_DOINT )) {
		if (UseDeferredInterruptModel() && trace_irqmanaged_env()) {
			static unsigned long doint_pre_count = 0;
			if (doint_pre_count < 4000) {
				MakeSR();
				fprintf(stderr, "IRQM spec-doint pre %lu pc=%08x sr=%04x intmask=%u spc=%08x live=%08x\n",
					++doint_pre_count,
					(unsigned)m68k_getpc(), (unsigned)regs.sr,
					(unsigned)regs.intmask, (unsigned)regs.spcflags,
					(unsigned)InterruptFlags);
			}
		}
		SPCFLAGS_CLEAR( SPCFLAG_DOINT );
		int intr = intlev ();
		if (UseDeferredInterruptModel() && trace_irqmanaged_env()) {
			static unsigned long doint_post_count = 0;
			if (doint_post_count < 4000) {
				MakeSR();
				fprintf(stderr, "IRQM spec-doint post %lu pc=%08x sr=%04x intmask=%u spc=%08x intr=%d live=%08x\n",
					++doint_post_count,
					(unsigned)m68k_getpc(), (unsigned)regs.sr,
					(unsigned)regs.intmask, (unsigned)regs.spcflags,
					intr, (unsigned)InterruptFlags);
			}
		}
		if (intr != -1 && intr > regs.intmask) {
			Interrupt (intr);
			if (UseDeferredInterruptModel())
				SPCFLAGS_CLEAR( SPCFLAG_INT | SPCFLAG_DOINT );
			regs.stopped = 0;
		}
	}

	if (SPCFLAGS_TEST( SPCFLAG_INT )) {
		if (UseDeferredInterruptModel() && trace_irqmanaged_env()) {
			static unsigned long promote_count = 0;
			if (promote_count < 4000) {
				MakeSR();
				fprintf(stderr, "IRQM spec-promote %lu pc=%08x sr=%04x intmask=%u spc=%08x live=%08x\n",
					++promote_count,
					(unsigned)m68k_getpc(), (unsigned)regs.sr,
					(unsigned)regs.intmask, (unsigned)regs.spcflags,
					(unsigned)InterruptFlags);
			}
		}
		SPCFLAGS_CLEAR( SPCFLAG_INT );
		SPCFLAGS_SET( SPCFLAG_DOINT );
	}

	if (SPCFLAGS_TEST( SPCFLAG_BRK /*| SPCFLAG_MODE_CHANGE*/ )) {
		SPCFLAGS_CLEAR( SPCFLAG_BRK /*| SPCFLAG_MODE_CHANGE*/ );
		return 1;
	}

	return 0;
}

void m68k_do_execute (void)
{
    uae_u32 pc;
    uae_u32 opcode;
    static unsigned long nojit_insn_count = 0;
    static unsigned long nojit_last_report_count = 0;
    static uae_u32 nojit_pc_min = 0xFFFFFFFF;
    static uae_u32 nojit_pc_max = 0;
    static unsigned long trace_window_count = 0;
    for (;;) {
#ifdef USE_JIT
	if (!UseJIT)
		SPCFLAGS_CLEAR(SPCFLAG_JIT_END_COMPILE | SPCFLAG_JIT_EXEC_RETURN);
#endif
	regs.fault_pc = pc = m68k_getpc();
	nojit_insn_count++;
	if (pc < nojit_pc_min) nojit_pc_min = pc;
	if (pc > nojit_pc_max) nojit_pc_max = pc;
	if ((nojit_insn_count - nojit_last_report_count) >= 5000000) {
	    nojit_last_report_count = nojit_insn_count;
	    fprintf(stderr, "NOJIT_DIAG insn=%lu pc=0x%08x range=[0x%08x,0x%08x] sr=0x%04x d0=0x%08x a7=0x%08x\n",
	        nojit_insn_count, pc, nojit_pc_min, nojit_pc_max, (unsigned)regs.sr,
	        (unsigned)m68k_dreg(regs,0), (unsigned)m68k_areg(regs,7));
	    nojit_pc_min = 0xFFFFFFFF;
	    nojit_pc_max = 0;
	}
#ifdef FULL_HISTORY
#ifdef NEED_TO_DEBUG_BADLY
	history[lasthist] = regs;
	historyf[lasthist] =  regflags;
#else
	history[lasthist] = m68k_getpc();
#endif
	if (++lasthist == MAX_HIST) lasthist = 0;
	if (lasthist == firsthist) {
	    if (++firsthist == MAX_HIST) firsthist = 0;
	}
#endif

#ifndef FULLMMU
#ifdef ARAM_PAGE_CHECK
	if (((pc ^ pc_page) > ARAM_PAGE_MASK)) {
	    check_ram_boundary(pc, 2, false);
	    pc_page = pc;
	    pc_offset = (uintptr)get_real_address(pc, 0, sz_word) - pc;
	}
#else
	check_ram_boundary(pc, 2, false);
#endif
#endif
	opcode = GET_OPCODE;
	trace_recent_record(pc, opcode);
	bool trace_this_step = false;
	if (trace_window_count < trace_window_limit() && trace_window_matches(pc)) {
		trace_this_step = true;
		trace_window_log("BEFORE", trace_window_count + 1, pc, opcode);
	}
#ifdef FLIGHT_RECORDER
	m68k_record_step(m68k_getpc(), cft_map(opcode));
#endif
	(*cpufunctbl[opcode])(opcode);
	if (trace_this_step) {
		trace_window_count++;
		trace_window_log("AFTER", trace_window_count, m68k_getpc(), opcode);
	}

	/* Flag divergence trace */
	{
		static unsigned long _ft_count = 0;
		_ft_count++;
		if (_ft_count <= 2000) {
			MakeSR();
			fprintf(stderr, "FT %lu %08x %04x %04x D1=%08x D3=%08x\n",
				_ft_count, (unsigned)m68k_getpc(), (unsigned)opcode, (unsigned)regs.sr, (unsigned)m68k_dreg(regs,1), (unsigned)m68k_dreg(regs,3));
		}
	}
	/* Targeted path-selection trace for the intermittent L2 startup split.
	   Enable with B2_TRACE_PATHSEL=1 to log the exact guest state around the
	   0x040099b0..0x04009a30 dispatcher and the related low-memory interrupt /
	   scheduler region at 0x0400a298..0x0400a2f8. */
	{
		static int _ps_init = 0;
		static int _ps_enabled = 0;
		static unsigned long _ps_count = 0;
		if (!_ps_init) {
			const char *env = getenv("B2_TRACE_PATHSEL");
			_ps_enabled = (env && env[0] && strcmp(env, "0") != 0) ? 1 : 0;
			_ps_init = 1;
		}
		if (_ps_enabled) {
			uaecptr _ps_pc = m68k_getpc();
			if (((_ps_pc >= 0x040099b0 && _ps_pc <= 0x04009a30) ||
				 (_ps_pc >= 0x0400a298 && _ps_pc <= 0x0400a2f8)) &&
				_ps_count < 2000) {
				MakeSR();
				fprintf(stderr,
					"PS %lu pc=%08x op=%04x sr=%04x D0=%08x D1=%08x D2=%08x A0=%08x A1=%08x A2=%08x A7=%08x LM110=%08x LM160=%02x LM162=%08x LM16A=%08x LM2AA=%08x\n",
					++_ps_count,
					(unsigned)_ps_pc,
					(unsigned)opcode,
					(unsigned)regs.sr,
					(unsigned)m68k_dreg(regs, 0),
					(unsigned)m68k_dreg(regs, 1),
					(unsigned)m68k_dreg(regs, 2),
					(unsigned)m68k_areg(regs, 0),
					(unsigned)m68k_areg(regs, 1),
					(unsigned)m68k_areg(regs, 2),
					(unsigned)m68k_areg(regs, 7),
					(unsigned)ReadMacInt32(0x110),
					(unsigned)ReadMacInt8(0x160),
					(unsigned)ReadMacInt32(0x162),
					(unsigned)ReadMacInt32(0x16a),
					(unsigned)ReadMacInt32(0x2aa));
			}
		}
	}

	cpu_check_ticks();
	regs.fault_pc = m68k_getpc();

	if (SPCFLAGS_TEST(SPCFLAG_ALL_BUT_EXEC_RETURN)) {
		if (m68k_do_specialties())
			return;
	}
    }
}

void m68k_execute (void)
{
#ifdef USE_JIT
    m68k_execute_depth++;
#endif
#ifdef DEBUGGER
    VOLATILE bool after_exception = false;
#endif

setjmpagain:
    TRY(prb) {
	for (;;) {
	    if (quit_program > 0) {
		if (quit_program == 1) {
#ifdef FLIGHT_RECORDER
		    dump_flight_recorder();
#endif
		    break;
		}
		quit_program = 0;
		m68k_reset ();
	    }
#ifdef DEBUGGER
	    if (debugging && !after_exception) debug();
	    after_exception = false;
#endif
	    m68k_do_execute();
	}
    }
    CATCH(prb) {
        Exception(prb, 0);
#ifdef DEBUGGER
	after_exception = true;
#endif
    	goto setjmpagain;
    }

#ifdef USE_JIT
    m68k_execute_depth--;
#endif
}

void m68k_disasm (FILE *f, uaecptr addr, uaecptr *nextpc, int cnt)
{
#ifdef HAVE_DISASM_M68K
	char buf[256];
	int size;

	disasm_info.memory_vma = addr;
    while (cnt-- > 0) {
		size = m68k_disasm_to_buf(&disasm_info, buf, 1);
    	fprintf(f, "%s\n", buf);
    	if (size < 0)
    		break;
	}
    if (nextpc)
		*nextpc = disasm_info.memory_vma;
#else
    if (nextpc)
		*nextpc = addr;
	(void) f;
	(void) cnt;
#endif
}

#ifdef DEBUGGER
void newm68k_disasm(FILE *f, uaecptr addr, uaecptr *nextpc, unsigned int cnt)
{
#ifdef HAVE_DISASM_M68K
	char buf[256];

	disasm_info.memory_vma = addr;
    if (cnt == 0) {
		m68k_disasm_to_buf(&disasm_info, buf, 1);
    } else {
	    while (cnt-- > 0) {
		m68k_disasm_to_buf(&disasm_info, buf, 1);
    	fprintf(f, "%s\n", buf);
    	}
    }
    if (nextpc)
		*nextpc = disasm_info.memory_vma;
#else
    if (nextpc)
		*nextpc = addr;
	(void) cnt;
#endif
}

#endif /* DEBUGGER */

#ifdef FULL_HISTORY
void showDisasm(uaecptr addr) {
#ifdef HAVE_DISASM_M68K
	char buf[256];

	disasm_info.memory_vma = addr;
	m68k_disasm_to_buf(&disasm_info, buf, 1);
	bug("%s", buf);
#else
	(void) addr;
#endif
}
#endif /* FULL_HISTORY */

void m68k_dumpstate (FILE *out, uaecptr *nextpc)
{
    int i;
    for (i = 0; i < 8; i++){
	fprintf (out, "D%d: %08lx ", i, (unsigned long)m68k_dreg(regs, i));
	if ((i & 3) == 3) fprintf (out, "\n");
    }
    for (i = 0; i < 8; i++){
	fprintf (out, "A%d: %08lx ", i, (unsigned long)m68k_areg(regs, i));
	if ((i & 3) == 3) fprintf (out, "\n");
    }
    if (regs.s == 0) regs.usp = m68k_areg(regs, 7);
    if (regs.s && regs.m) regs.msp = m68k_areg(regs, 7);
    if (regs.s && regs.m == 0) regs.isp = m68k_areg(regs, 7);
    fprintf (out, "USP=%08lx ISP=%08lx MSP=%08lx VBR=%08lx\n",
	    (unsigned long)regs.usp, (unsigned long)regs.isp,
	    (unsigned long)regs.msp, (unsigned long)regs.vbr);
    fprintf (out, "T=%d%d S=%d M=%d X=%d N=%d Z=%d V=%d C=%d IMASK=%d TCE=%d TCP=%d\n",
	    regs.t1, regs.t0, regs.s, regs.m,
	    (int)GET_XFLG(), (int)GET_NFLG(), (int)GET_ZFLG(), (int)GET_VFLG(), (int)GET_CFLG(), regs.intmask,
	    regs.mmu_enabled, regs.mmu_pagesize_8k);
    fprintf (out, "CACR=%08lx CAAR=%08lx  URP=%08lx  SRP=%08lx\n",
            (unsigned long)regs.cacr,
	    (unsigned long)regs.caar,
	    (unsigned long)regs.urp,
	    (unsigned long)regs.srp);
    fprintf (out, "DTT0=%08lx DTT1=%08lx ITT0=%08lx ITT1=%08lx\n",
            (unsigned long)regs.dtt0,
	    (unsigned long)regs.dtt1,
	    (unsigned long)regs.itt0,
	    (unsigned long)regs.itt1);
    for (i = 0; i < 8; i++){
	fprintf (out, "FP%d: %g ", i, (double)fpu.registers[i]);
	if ((i & 3) == 3) fprintf (out, "\n");
    }
#if 0
    fprintf (out, "N=%d Z=%d I=%d NAN=%d\n",
		(regs.fpsr & 0x8000000) != 0,
		(regs.fpsr & 0x4000000) != 0,
		(regs.fpsr & 0x2000000) != 0,
		(regs.fpsr & 0x1000000) != 0);
#endif
    m68k_disasm(out, m68k_getpc (), nextpc, 1);
    if (nextpc)
	fprintf (out, "next PC: %08lx\n", (unsigned long)*nextpc);
}
