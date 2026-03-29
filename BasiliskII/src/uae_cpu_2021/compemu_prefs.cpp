#include "sysdeps.h"
#include "prefs.h"

#ifndef BASILISK_UAE_PREFS_COMPAT_DEFINED
#define BASILISK_UAE_PREFS_COMPAT_DEFINED
struct uae_prefs {
	int comptrustbyte;
	int comptrustword;
	int comptrustlong;
	int comptrustnaddr;
	bool compnf;
	bool compfpu;
	bool comp_hardflush;
	int comp_constjump;
	int cachesize;
	int m68k_speed;
	int cpu_model;
	int cpu_level;
	bool cpu_compatible;
	bool illegal_mem;
	bool address_space_24;
};
#endif

uae_prefs currprefs = {};
uae_prefs changed_prefs = {};
bool canbang = true;

extern int special_mem_default;

static int pref_cpu_to_model(int cpu_pref)
{
	switch (cpu_pref) {
	case 0: return 68000;
	case 1: return 68010;
	case 2: return 68020;
	case 3: return 68030;
	case 4: return 68040;
	default:
		return cpu_pref >= 68000 ? cpu_pref : 68030;
	}
}

static int pref_cpu_to_level(int cpu_model)
{
	switch (cpu_model) {
	case 68000: return 0;
	case 68010: return 1;
	case 68020: return 2;
	case 68030: return 3;
	default:
		return cpu_model >= 68040 ? 5 : 3;
	}
}

static void sync_jit_prefs(uae_prefs &p)
{
	const bool jit_enabled = PrefsFindBool("jit");
	p.cachesize = jit_enabled ? PrefsFindInt32("jitcachesize") : 0;
	p.compfpu = PrefsFindBool("jitfpu");
	p.comp_hardflush = !PrefsFindBool("jitlazyflush");
	p.comp_constjump = PrefsFindBool("jitinline");
	p.compnf = true;
	p.m68k_speed = -1;
	p.cpu_model = pref_cpu_to_model(PrefsFindInt32("cpu"));
	p.cpu_level = pref_cpu_to_level(p.cpu_model);
	p.cpu_compatible = false;  /* AArch64: native block dispatch, interpreter opcodes (optlev capped at 1) */
	p.illegal_mem = false;
	p.address_space_24 = false;

	const int distrust = (!jit_enabled || !canbang) ? 1 : 0;
	p.comptrustbyte = distrust;
	p.comptrustword = distrust;
	p.comptrustlong = distrust;
	p.comptrustnaddr = distrust;
}

bool check_prefs_changed_comp(bool checkonly)
{
	uae_prefs latest = currprefs;
	sync_jit_prefs(latest);

	const bool changed =
		latest.comptrustbyte != currprefs.comptrustbyte ||
		latest.comptrustword != currprefs.comptrustword ||
		latest.comptrustlong != currprefs.comptrustlong ||
		latest.comptrustnaddr != currprefs.comptrustnaddr ||
		latest.compnf != currprefs.compnf ||
		latest.compfpu != currprefs.compfpu ||
		latest.comp_hardflush != currprefs.comp_hardflush ||
		latest.comp_constjump != currprefs.comp_constjump ||
		latest.cachesize != currprefs.cachesize ||
		latest.cpu_model != currprefs.cpu_model ||
		latest.cpu_level != currprefs.cpu_level ||
		latest.cpu_compatible != currprefs.cpu_compatible ||
		latest.address_space_24 != currprefs.address_space_24;

	if (checkonly)
		return changed;

	currprefs = latest;
	changed_prefs = latest;
	special_mem_default = currprefs.comptrustbyte ? (1 | 2 | 4) : 0;
	return changed;
}

struct jit_prefs_bootstrap {
	jit_prefs_bootstrap() {
		sync_jit_prefs(currprefs);
		changed_prefs = currprefs;
	}
} static const jit_prefs_bootstrap_instance;
