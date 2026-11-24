/*
 * cpu_exec1's body (i386.c includes it two or three times; see EXEC1_HOT
 * there).
 *
 * EXEC1_NAME the function, EXEC1_ATTR its placement, EXEC1_TAB_ATTR its
 * prefix table's. EXEC1_JIT: the JIT's hook at the loop head; EXEC1_COMPACT:
 * groups 1-5 compactly (the SRAM copy that shares SRAM with the JIT). EXEC1_HOT 1: the SRAM copy, in which every opcode of the
 * COLD set (exec1_cold.h) goes to the stub at the loop's end, which runs that
 * one instruction in the flash copy (EXEC1_HOT 0, every opcode). Nothing has
 * changed when a handler starts (prefixes only set locals), so the flash copy
 * runs the instruction from its first byte.
 */
static bool EXEC1_ATTR EXEC1_NAME(CPUI386 *cpu, int stepcount)
{
#ifndef I386_OPT2
#define eswitch(b) switch(b)
#define ecase(a)   case a
#define ebreak     break
#define edefault   default
#define default_ud cpu_debug(cpu); THROW0(EX_UD)
#undef CX
#define CX(_1) case _1:
#else
#define eswitch(b)
#define ecase(a)   f ## a
#define ebreak     continue
#define edefault   f0xf1
#define default_ud THROW0(EX_UD)
#undef CX
#define CX(_1) f ## _1:
#endif

	u8 b1;
	u8 modrm;
	OptAddr meml;
	uword addr;
#if EXEC1_JIT
	uword jit_last = cpu->next_ip - 16;     /* (a lookup at the slice's start) */
	bool jit_resume = false, jit_new = false;
#endif
	for (; stepcount > 0; stepcount--) {
	bool code16 = cpu->code16;
	uword sp_mask = cpu->sp_mask;

	if (code16) cpu->next_ip &= 0xffff;
#if EXEC1_JIT
	/* a loop head (IP back, or on by more than an instruction): a compiled
	 * block there runs instead (jit16.inc) */
	if (unlikely(cpu->next_ip - jit_last > 15u)) {
		bool stop;
		int jn = jit_enter(cpu, stepcount, jit_new, &stop);
		jit_new = true;
		jit_resume = stop;
		if (jn > 0) {
			cpu->cycle += jn;
			stepcount -= jn - 1;
			/* where it went: another lookup; or at an instruction a block
			 * stopped before, this one in the interpreter, then a lookup
			 * after it (else the code after it stayed interpreted to the
			 * next jump) */
			jit_last = stop ? cpu->next_ip : cpu->next_ip - 16;
			/* (as the handlers' early returns: HLT, an interrupt let in) */
			if (cpu->halt || cpu->stop || (cpu->intr && (cpu->flags & IF)))
				return true;
			continue;
		}
	}
	jit_last = jit_resume ? cpu->next_ip - 16 : cpu->next_ip;
	jit_resume = false;
	jit_new = true;
#endif
	cpu->ip = cpu->next_ip;
	uintptr_t fbase = fetch_base(cpu);
#if CPU_IP_RING
	cpu_ip_ring[cpu_ip_ring_pos++ & (CPU_IP_RING_N - 1)] = cpu->ip;
#endif
#if BB_PROFILE
	{ static uint32_t bb_prev; bb_note(cpu->ip, bb_prev); bb_prev = cpu->ip; }
#endif
	TRY(fetch8(cpu, &b1));
#if EXEC1_JIT && JIT_PROF
	{
		extern uint32_t jit_prof_op[256], jit_prof_pfx[JIT_PROF_PFX ? 3 : 1][256];
		if (!(cpu->cr0 & 1)) {
			jit_prof_op[b1]++;
			{	/* (where: a ring of linear addresses, bit 31 inside a block's step) */
				extern uint32_t jit_prof_lin[256], jit_prof_lpos; extern bool jit_prof_instep;
				jit_prof_lin[jit_prof_lpos++ & 255] = (cpu->seg[SEG_CS].base + cpu->ip) | (uint32_t)jit_prof_instep << 31;
			}
			/* (after a 66, 67 or 26 prefix: the bytes after all the prefixes) */
			int t = JIT_PROF_PFX ? (b1 == 0x66 ? 0 : b1 == 0x67 ? 1 : b1 == 0x26 ? 2 : -1) : -1;
			if (t >= 0) {
				const u8 *q = cpu->phys_mem + cpu->seg[SEG_CS].base + cpu->ip;
				int i = 1;
				while (i < 5 && (q[i] == 0x66 || q[i] == 0x67 || q[i] == 0x26 || q[i] == 0x2e ||
						 q[i] == 0x36 || q[i] == 0x3e || q[i] == 0x64 || q[i] == 0x65 ||
						 q[i] == 0xf2 || q[i] == 0xf3))
					i++;
				jit_prof_pfx[t][q[i] == 0x0f ? q[i + 1] : q[i]]++;
			}
		}
	}
#endif
#if DEBUG_CPU
	opcode = b1;
#endif
	cpu->cycle++;

#ifndef I386_OPT1
	if (verbose) {
		cpu_debug(cpu);
	}
#endif
	// prefix
	bool opsz16 = code16;
	bool adsz16 = code16;
	int rep = 0;
	/*bool lock = false;*/
	int curr_seg = -1;
#ifndef I386_OPT2
	for (;;) {
#define HANDLE_PREFIX(C, STMT) \
		if (b1 == C) { \
			STMT; \
			TRY(fetch8(cpu, &b1)); \
			continue; \
		}
		HANDLE_PREFIX(0x26, curr_seg = SEG_ES)
		HANDLE_PREFIX(0x2e, curr_seg = SEG_CS)
		HANDLE_PREFIX(0x36, curr_seg = SEG_SS)
		HANDLE_PREFIX(0x3e, curr_seg = SEG_DS)
		HANDLE_PREFIX(0x64, curr_seg = SEG_FS)
		HANDLE_PREFIX(0x65, curr_seg = SEG_GS)
		HANDLE_PREFIX(0x66, opsz16 = !code16)
		HANDLE_PREFIX(0x67, adsz16 = !code16)
		HANDLE_PREFIX(0xf3, rep = 1) // REP
		HANDLE_PREFIX(0xf2, rep = 2) // REPNE
		HANDLE_PREFIX(0xf0, /*lock = true*/)
#undef HANDLE_PREFIX
		break;
	}
#else
	static const void *const pfxlabel[] EXEC1_TAB_ATTR = {
/* 0x00 */	&&f0x00, &&f0x01, &&f0x02, &&f0x03, &&f0x04, &&f0x05, &&f0x06, &&f0x07,
/* 0x08 */	&&f0x08, &&f0x09, &&f0x0a, &&f0x0b, &&f0x0c, &&f0x0d, &&f0x0e, &&f0x0f,
/* 0x10 */	&&f0x10, &&f0x11, &&f0x12, &&f0x13, &&f0x14, &&f0x15, &&f0x16, &&f0x17,
/* 0x18 */	&&f0x18, &&f0x19, &&f0x1a, &&f0x1b, &&f0x1c, &&f0x1d, &&f0x1e, &&f0x1f,
/* 0x20 */	&&f0x20, &&f0x21, &&f0x22, &&f0x23, &&f0x24, &&f0x25, &&pfx26, &&f0x27,
/* 0x28 */	&&f0x28, &&f0x29, &&f0x2a, &&f0x2b, &&f0x2c, &&f0x2d, &&pfx2e, &&f0x2f,
/* 0x30 */	&&f0x30, &&f0x31, &&f0x32, &&f0x33, &&f0x34, &&f0x35, &&pfx36, &&f0x37,
/* 0x38 */	&&f0x38, &&f0x39, &&f0x3a, &&f0x3b, &&f0x3c, &&f0x3d, &&pfx3e, &&f0x3f,
/* 0x40 */	&&f0x40, &&f0x41, &&f0x42, &&f0x43, &&f0x44, &&f0x45, &&f0x46, &&f0x47,
/* 0x48 */	&&f0x48, &&f0x49, &&f0x4a, &&f0x4b, &&f0x4c, &&f0x4d, &&f0x4e, &&f0x4f,
/* 0x50 */	&&f0x50, &&f0x51, &&f0x52, &&f0x53, &&f0x54, &&f0x55, &&f0x56, &&f0x57,
/* 0x58 */	&&f0x58, &&f0x59, &&f0x5a, &&f0x5b, &&f0x5c, &&f0x5d, &&f0x5e, &&f0x5f,
/* 0x60 */	&&f0x60, &&f0x61, &&f0x62, &&f0x63, &&pfx64, &&pfx65, &&pfx66, &&pfx67,
/* 0x68 */	&&f0x68, &&f0x69, &&f0x6a, &&f0x6b, &&f0x6c, &&f0x6d, &&f0x6e, &&f0x6f,
/* 0x70 */	&&f0x70, &&f0x71, &&f0x72, &&f0x73, &&f0x74, &&f0x75, &&f0x76, &&f0x77,
/* 0x78 */	&&f0x78, &&f0x79, &&f0x7a, &&f0x7b, &&f0x7c, &&f0x7d, &&f0x7e, &&f0x7f,
/* 0x80 */	&&f0x80, &&f0x81, &&f0x82, &&f0x83, &&f0x84, &&f0x85, &&f0x86, &&f0x87,
/* 0x88 */	&&f0x88, &&f0x89, &&f0x8a, &&f0x8b, &&f0x8c, &&f0x8d, &&f0x8e, &&f0x8f,
/* 0x90 */	&&f0x90, &&f0x91, &&f0x92, &&f0x93, &&f0x94, &&f0x95, &&f0x96, &&f0x97,
/* 0x98 */	&&f0x98, &&f0x99, &&f0x9a, &&f0x9b, &&f0x9c, &&f0x9d, &&f0x9e, &&f0x9f,
/* 0xa0 */	&&f0xa0, &&f0xa1, &&f0xa2, &&f0xa3, &&f0xa4, &&f0xa5, &&f0xa6, &&f0xa7,
/* 0xa8 */	&&f0xa8, &&f0xa9, &&f0xaa, &&f0xab, &&f0xac, &&f0xad, &&f0xae, &&f0xaf,
/* 0xb0 */	&&f0xb0, &&f0xb1, &&f0xb2, &&f0xb3, &&f0xb4, &&f0xb5, &&f0xb6, &&f0xb7,
/* 0xb8 */	&&f0xb8, &&f0xb9, &&f0xba, &&f0xbb, &&f0xbc, &&f0xbd, &&f0xbe, &&f0xbf,
/* 0xc0 */	&&f0xc0, &&f0xc1, &&f0xc2, &&f0xc3, &&f0xc4, &&f0xc5, &&f0xc6, &&f0xc7,
/* 0xc8 */	&&f0xc8, &&f0xc9, &&f0xca, &&f0xcb, &&f0xcc, &&f0xcd, &&f0xce, &&f0xcf,
/* 0xd0 */	&&f0xd0, &&f0xd1, &&f0xd2, &&f0xd3, &&f0xd4, &&f0xd5, &&f0xd6, &&f0xd7,
/* 0xd8 */	&&f0xd8, &&f0xd9, &&f0xda, &&f0xdb, &&f0xdc, &&f0xdd, &&f0xde, &&f0xdf,
/* 0xe0 */	&&f0xe0, &&f0xe1, &&f0xe2, &&f0xe3, &&f0xe4, &&f0xe5, &&f0xe6, &&f0xe7,
/* 0xe8 */	&&f0xe8, &&f0xe9, &&f0xea, &&f0xeb, &&f0xec, &&f0xed, &&f0xee, &&f0xef,
/* 0xf0 */	&&pfxf0, &&f0xf1, &&pfxf2, &&pfxf3, &&f0xf4, &&f0xf5, &&f0xf6, &&f0xf7,
/* 0xf8 */	&&f0xf8, &&f0xf9, &&f0xfa, &&f0xfb, &&f0xfc, &&f0xfd, &&f0xfe, &&f0xff,
	};
	goto *pfxlabel[b1];
#define HANDLE_PREFIX(C, STMT) \
		pfx ## C: { \
			STMT; \
			TRY(fetch8(cpu, &b1)); \
			goto *pfxlabel[b1]; \
		}
		HANDLE_PREFIX(26, curr_seg = SEG_ES)
		HANDLE_PREFIX(2e, curr_seg = SEG_CS)
		HANDLE_PREFIX(36, curr_seg = SEG_SS)
		HANDLE_PREFIX(3e, curr_seg = SEG_DS)
		HANDLE_PREFIX(64, curr_seg = SEG_FS)
		HANDLE_PREFIX(65, curr_seg = SEG_GS)
		HANDLE_PREFIX(66, opsz16 = !code16)
		HANDLE_PREFIX(67, adsz16 = !code16)
		HANDLE_PREFIX(f3, rep = 1) // REP
		HANDLE_PREFIX(f2, rep = 2) // REPNE
		HANDLE_PREFIX(f0, /*lock = true*/)
#undef HANDLE_PREFIX
#endif
	eswitch(b1) {
#define I(_case, _rm, _rwm, _op) _case { EXEC1_COLD_GUARD(_op) _rm(_rwm, _op); ebreak; }
#include "i386ins.def"
#undef I

#undef CX
#define CX(_1) case _1:
#define GRPBEG TRY(peek8(cpu, &modrm)); switch((modrm >> 3) & 7) {
#define GRPCASE(_case, _rm, _rwm, _op) _case { EXEC1_COLD_GUARD(_op) _rm(_rwm, _op); ebreak; }
#define GRPEND default: default_ud; } ebreak;

#if EXEC1_COMPACT
	/* group 1 compactly (alu_grp1, i386.c): the flash copy below has the
	 * expansions, in the same order of fetches and translation */
	ecase(0x80): ecase(0x81): ecase(0x82): ecase(0x83): {
		TRY(fetch8(cpu, &modrm));
		int mod = modrm >> 6, rm = modrm & 7, op = (modrm >> 3) & 7;
		int bits = !(b1 & 1) ? 8 : opsz16 ? 16 : 32;
		if (mod != 3)
			TRY(modsib(cpu, adsz16, mod, rm, &addr, &curr_seg));
		uword imm;
		if (b1 == 0x81 && bits == 16) { u16 i; TRY(fetch16(cpu, &i)); imm = i; }
		else if (b1 == 0x81) { u32 i; TRY(fetch32(cpu, &i)); imm = i; }
		else { u8 i; TRY(fetch8(cpu, &i)); imm = b1 == 0x83 ? (uword)(sword)(s8)i : i; }
		uword x, r;
		if (mod == 3) {
			x = bits == 8 ? lreg8(rm) : bits == 16 ? lreg16(rm) : lreg32(rm);
			r = alu_grp1(cpu, op, x, imm, bits);
			if (op != 7) {
				if (bits == 8) sreg8(rm, r); else if (bits == 16) sreg16(rm, r); else sreg32(rm, r);
			}
		} else if (bits == 8) {
			TRY(translate8(cpu, &meml, op == 7 ? 1 : 3, curr_seg, addr));
			r = alu_grp1(cpu, op, laddr8(&meml), imm, 8);
			if (op != 7) saddr8(&meml, r);
		} else if (bits == 16) {
			TRY(translate16(cpu, &meml, op == 7 ? 1 : 3, curr_seg, addr));
			r = alu_grp1(cpu, op, laddr16(&meml), imm, 16);
			if (op != 7) saddr16(&meml, r);
		} else {
			TRY(translate32(cpu, &meml, op == 7 ? 1 : 3, curr_seg, addr));
			r = alu_grp1(cpu, op, laddr32(&meml), imm, 32);
			if (op != 7) saddr32(&meml, r);
		}
		ebreak;
	}
#else
	ecase(0x80): ecase(0x82): { // G1b
GRPBEG
#define IG1b GRPCASE
#include "i386ins.def"
#undef IG1b
GRPEND
	}

	ecase(0x81): { // G1v
GRPBEG
#define IG1v GRPCASE
#include "i386ins.def"
#undef IG1v
GRPEND
	}

	ecase(0x83): { // G1vIb
GRPBEG
#define IG1vIb GRPCASE
#include "i386ins.def"
#undef IG1vIb
GRPEND
	}
#endif

#if EXEC1_COMPACT
	/* group 2 compactly: each operation's macro once per width, on a local
	 * (the flash copy below has the expansions; RCL/RCR are its) */
	ecase(0xc0): ecase(0xc1): ecase(0xd0): ecase(0xd1): ecase(0xd2): ecase(0xd3): {
		TRY(fetch8(cpu, &modrm));
		int mod = modrm >> 6, rm = modrm & 7, op = (modrm >> 3) & 7;
		if (op == 2 || op == 3)
			goto exec1_cold;
		int bits = !(b1 & 1) ? 8 : opsz16 ? 16 : 32;
		if (mod != 3)
			TRY(modsib(cpu, adsz16, mod, rm, &addr, &curr_seg));
		u8 cnt = 1;
		if (b1 <= 0xc1) {
			TRY(fetch8(cpu, &cnt));
		} else if (b1 >= 0xd2)
			cnt = lreg8(1);
		uword v;
		if (mod == 3)
			v = bits == 8 ? lreg8(rm) : bits == 16 ? lreg16(rm) : lreg32(rm);
		else if (bits == 8) { TRY(translate8(cpu, &meml, 3, curr_seg, addr)); v = laddr8(&meml); }
		else if (bits == 16) { TRY(translate16(cpu, &meml, 3, curr_seg, addr)); v = laddr16(&meml); }
		else { TRY(translate32(cpu, &meml, 3, curr_seg, addr)); v = laddr32(&meml); }
		bool wr = false;
#define G2_LB(a) ((u8)v)
#define G2_LW(a) ((u16)v)
#define G2_LD(a) ((u32)v)
#define G2_SB(a, x) (v = (u8)(x), wr = true)
#define G2_SW(a, x) (v = (u16)(x), wr = true)
#define G2_SD(a, x) (v = (u32)(x), wr = true)
#define G2(BW, la, sa) \
		switch (op) { \
		case 0: { ROL ## BW(0, cnt, la, sa, limm, 0) break; } \
		case 1: { ROR ## BW(0, cnt, la, sa, limm, 0) break; } \
		case 5: { SHR ## BW(0, cnt, la, sa, limm, 0) break; } \
		case 7: { SAR ## BW(0, cnt, la, sa, limm, 0) break; } \
		default: { SHL ## BW(0, cnt, la, sa, limm, 0) break; } \
		}
		if (bits == 8) { G2(b, G2_LB, G2_SB) } else if (bits == 16) { G2(w, G2_LW, G2_SW) } else { G2(d, G2_LD, G2_SD) }
#undef G2
#undef G2_LB
#undef G2_LW
#undef G2_LD
#undef G2_SB
#undef G2_SW
#undef G2_SD
		if (wr) {
			if (mod == 3) {
				if (bits == 8) sreg8(rm, v); else if (bits == 16) sreg16(rm, v); else sreg32(rm, v);
			} else if (bits == 8) saddr8(&meml, v);
			else if (bits == 16) saddr16(&meml, v);
			else saddr32(&meml, v);
		}
		ebreak;
	}
#else
	ecase(0xc0): { // G2b
GRPBEG
#define IG2b GRPCASE
#include "i386ins.def"
#undef IG2b
GRPEND
	}

	ecase(0xc1): { // G2v
GRPBEG
#define IG2v GRPCASE
#include "i386ins.def"
#undef IG2v
GRPEND
	}

	ecase(0xd0): { // G2b1
GRPBEG
#define IG2b1 GRPCASE
#include "i386ins.def"
#undef IG2b1
GRPEND
	}

	ecase(0xd1): { // G2v1
GRPBEG
#define IG2v1 GRPCASE
#include "i386ins.def"
#undef IG2v1
GRPEND
	}

	ecase(0xd2): { // G2bC
GRPBEG
#define IG2bC GRPCASE
#include "i386ins.def"
#undef IG2bC
GRPEND
	}

	ecase(0xd3): { // G2v1
GRPBEG
#define IG2vC GRPCASE
#include "i386ins.def"
#undef IG2vC
GRPEND
	}

#endif

#if EXEC1_COMPACT
	/* groups 3, 4, 5 compactly, as group 2 (far CALL/JMP: the flash copy's) */
#define GX_LB(a) ((u8)v)
#define GX_LW(a) ((u16)v)
#define GX_LD(a) ((u32)v)
#define GX_SB(a, x) (v = (u8)(x), wr = true)
#define GX_SW(a, x) (v = (u16)(x), wr = true)
#define GX_SD(a, x) (v = (u32)(x), wr = true)
#define GX_LOAD(rwm) \
		if (mod == 3) \
			v = bits == 8 ? lreg8(rm) : bits == 16 ? lreg16(rm) : lreg32(rm); \
		else if (bits == 8) { TRY(translate8(cpu, &meml, rwm, curr_seg, addr)); v = laddr8(&meml); } \
		else if (bits == 16) { TRY(translate16(cpu, &meml, rwm, curr_seg, addr)); v = laddr16(&meml); } \
		else { TRY(translate32(cpu, &meml, rwm, curr_seg, addr)); v = laddr32(&meml); }
#define GX_STORE \
		if (wr) { \
			if (mod == 3) { \
				if (bits == 8) sreg8(rm, v); else if (bits == 16) sreg16(rm, v); else sreg32(rm, v); \
			} else if (bits == 8) saddr8(&meml, v); \
			else if (bits == 16) saddr16(&meml, v); \
			else saddr32(&meml, v); \
		}
	ecase(0xf6): ecase(0xf7): {
		TRY(fetch8(cpu, &modrm));
		int mod = modrm >> 6, rm = modrm & 7, op = (modrm >> 3) & 7;
		int bits = !(b1 & 1) ? 8 : opsz16 ? 16 : 32;
		if (mod != 3)
			TRY(modsib(cpu, adsz16, mod, rm, &addr, &curr_seg));
		uword imm = 0;
		if (op < 2) {
			if (bits == 8) { u8 i; TRY(fetch8(cpu, &i)); imm = i; }
			else if (bits == 16) { u16 i; TRY(fetch16(cpu, &i)); imm = i; }
			else { u32 i; TRY(fetch32(cpu, &i)); imm = i; }
		}
		uword v;
		bool wr = false;
		GX_LOAD(op == 2 || op == 3 ? 3 : 1)
#define G3(BW, la, sa) \
		switch (op) { \
		case 0: case 1: { TEST ## BW(0, imm, la, sa, limm, 0) break; } \
		case 2: { NOT ## BW(0, la, sa) break; } \
		case 3: { NEG ## BW(0, la, sa) break; } \
		case 4: { MUL ## BW(0, la, sa) break; } \
		case 5: { IMUL ## BW(0, la, sa) break; } \
		case 6: { DIV ## BW(0, la, sa) break; } \
		default: { IDIV ## BW(0, la, sa) break; } \
		}
		if (bits == 8) { G3(b, GX_LB, GX_SB) } else if (bits == 16) { G3(w, GX_LW, GX_SW) } else { G3(d, GX_LD, GX_SD) }
#undef G3
		GX_STORE
		ebreak;
	}

	ecase(0xfe): ecase(0xff): {
		TRY(peek8(cpu, &modrm));
		int op = (modrm >> 3) & 7;
		if (op == 7 || (b1 == 0xfe && op > 1)) {
			default_ud;
		}
		if (op == 3 || op == 5)
			goto exec1_cold;
		TRY(fetch8(cpu, &modrm));
		int mod = modrm >> 6, rm = modrm & 7;
		int bits = b1 == 0xfe ? 8 : opsz16 ? 16 : 32;
		if (mod != 3)
			TRY(modsib(cpu, adsz16, mod, rm, &addr, &curr_seg));
		uword v;
		bool wr = false;
		GX_LOAD(op <= 1 ? 3 : 1)
#define G5(BW, la, sa) \
		switch (op) { \
		case 0: { INC ## BW(0, la, sa) break; } \
		case 1: { DEC ## BW(0, la, sa) break; } \
		case 2: { CALLABS ## BW(0, la, sa) break; } \
		case 4: { JMPABS ## BW(0, la, sa) break; } \
		default: { PUSH ## BW(0, la, sa) break; } \
		}
		if (bits == 8) {
			if (op == 0) { INCb(0, GX_LB, GX_SB) } else { DECb(0, GX_LB, GX_SB) }
		} else if (bits == 16) { G5(w, GX_LW, GX_SW) } else { G5(d, GX_LD, GX_SD) }
#undef G5
		GX_STORE
		ebreak;
	}
#undef GX_LOAD
#undef GX_STORE
#undef GX_LB
#undef GX_LW
#undef GX_LD
#undef GX_SB
#undef GX_SW
#undef GX_SD
#else
	ecase(0xf6): { // G3b
GRPBEG
#define IG3b GRPCASE
#include "i386ins.def"
#undef IG3b
GRPEND
	}

	ecase(0xf7): { // G3v
GRPBEG
#define IG3v GRPCASE
#include "i386ins.def"
#undef IG3v
GRPEND
	}

	ecase(0xfe): { // G4
GRPBEG
#define IG4 GRPCASE
#include "i386ins.def"
#undef IG4
GRPEND
	}

	ecase(0xff): { // G5
GRPBEG
#define IG5 GRPCASE
#include "i386ins.def"
#undef IG5
GRPEND
	}

#endif

	ecase(0x0f): { // two byte
		TRY(fetch8(cpu, &b1));
		switch(b1) {
#define I2(_case, _rm, _rwm, _op) _case { EXEC1_COLD_GUARD(_op) _rm(_rwm, _op); ebreak; }
#include "i386ins.def"
#undef I2

		case 0x00: { // G6
GRPBEG
#define IG6 GRPCASE
#include "i386ins.def"
#undef IG6
GRPEND
		}

		case 0x01: { // G7
GRPBEG
#define IG7 GRPCASE
#include "i386ins.def"
#undef IG7
GRPEND
		}

		case 0xba: { // G8
GRPBEG
#define IG8 GRPCASE
#include "i386ins.def"
#undef IG8
GRPEND
		}

		case 0xc7: { // G9
GRPBEG
#define IG9 GRPCASE
#include "i386ins.def"
#undef IG9
GRPEND
		}
		default: default_ud;
		}
		ebreak;
	}

	edefault: default_ud;
	}
#if EXEC1_HOT
	if (0) {
	exec1_cold:
		/* this instruction in the full copy (which counts it once more) */
		cpu->next_ip = cpu->ip;
		cpu->cycle--;
		if (!cpu_exec1_cold(cpu, 1))
			return false;
		/* the early returns of cpu_exec1's handlers: HLT, an interrupt let in */
		if (cpu->halt || cpu->stop || (cpu->intr && (cpu->flags & IF)))
			return true;
		continue;
	}
#endif
	}
	return true;
}
