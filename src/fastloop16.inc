/*
 * frank-dos: the fast loop for 16-bit code (included by i386.c, after
 * fastloop.inc, whose FX_MEM and lazy-flag helpers it shares).
 *
 * fastloop.inc takes flat 32-bit code. Real-mode programs, and much of a
 * DOS extender's own code, are 16-bit: segment bases, 16-bit operands and
 * addressing, IP wrapping at 64 KB. They all went through cpu_exec1, about
 * 100 M33 instructions each (Doom 286 at 153 cycles an instruction). This
 * loop takes what such code runs most (Doom 286's 90% is 54 opcodes, its
 * 99% 97: docs/BENCH.md) in the same way: IP as a host pointer into the code
 * page, decoding straight from it, memory operands through the TLB's tags,
 * the lazy flags exactly as cpu_exec1's macros leave them (tools/cpubench
 * compares the two, FZMODE=3).
 *
 * Anything else falls back to cpu_exec1 for one instruction, before
 * anything is changed: another opcode, a 66h or 67h prefix, an instruction
 * in the code page's last 15 bytes, a memory operand without a TLB tag for
 * the access (a page across, VGA memory, a page with compiled code), a
 * divide that raises #DE, a segment load the interpreter must do (SS: its
 * interrupt shadow; CS by MOV: #UD).
 *
 * Real mode only for now (16-bit protected mode loads descriptors).
 */
#ifndef FX16_ENABLE
#define FX16_ENABLE 0
#endif

#if FX16_ENABLE
static inline bool fx16_mode(CPUI386 *cpu)
{
	/* (TF: single steps are cpu_exec1's; V86 too, for now). SS 16 or
	 * 32 bits (spm: DOS/4GW's 16-bit code has a 32-bit stack). 16-bit
	 * protected mode as real mode, the segment loads cpu_exec1's
	 * (F16_PM_FALLBACK): DS, ES, SS not null, as fx_mode wants them
	 * (cpu_exec1's translate_fast checks no more than that either) */
	if (!cpu->code16 || (cpu->flags & (TF | VM)))
		return false;
	return !(cpu->cr0 & 1) ||
		(cpu->seg[SEG_DS].limit && cpu->seg[SEG_ES].limit && cpu->seg[SEG_SS].limit);
}

/* the instructions that load a segment register (and CLI, POPF: IOPL),
 * in protected mode: cpu_exec1's, before any of their side effects */
#define F16_PM_FALLBACK() do { \
	if (unlikely(cpu->cr0 & 1)) \
		goto fallback; \
} while (0)

/* ModRM (mod != 3), 16-bit addressing: the offset, and the segment it
 * defaults to (SS for BP-based forms); p past it on return */
static inline __attribute__((always_inline))
const u8 *fx16_ea(CPUI386 *cpu, const u8 *p, uword *ea, int *dseg)
{
	u8 m = *p++;
	int mod = m >> 6, rm = m & 7;
	uword a;
	if (mod == 0 && rm == 6) {
		a = *(u16u *)p;
		p += 2;
		*dseg = SEG_DS;
	} else {
		switch (rm) {
		case 0: a = REGi(3) + REGi(6); break;
		case 1: a = REGi(3) + REGi(7); break;
		case 2: a = REGi(5) + REGi(6); break;
		case 3: a = REGi(5) + REGi(7); break;
		case 4: a = REGi(6); break;
		case 5: a = REGi(7); break;
		case 6: a = REGi(5); break;
		default: a = REGi(3); break;
		}
		*dseg = (rm == 2 || rm == 3 || rm == 6) ? SEG_SS : SEG_DS;
		if (mod == 1)
			a += (s8)*p++;
		else if (mod == 2) {
			a += *(u16u *)p;
			p += 2;
		}
	}
	*ea = a & 0xffff;
	return p;
}

#define R16P(i) ((u16 *)&REGi(i))
/* a segment's base, an override's (ovr) before the default */
#define F16_SEGBASE(dseg) cpu->seg[ovr >= 0 ? ovr : (dseg)].base

/* the E operand of the ModRM at p: a register or memory */
#define F16_E(ptr, REGP, SIZE, W) do { \
	u8 _mr = *p; \
	if ((_mr >> 6) == 3) { \
		ptr = (u8 *)REGP(_mr & 7); \
		p++; \
	} else { \
		uword _ea; \
		int _ds; \
		p = fx16_ea(cpu, p, &_ea, &_ds); \
		FX_MEM(ptr, F16_SEGBASE(_ds) + _ea, SIZE, W); \
	} \
} while (0)

/* the stack: SS:SP, 16 bits (ESP's high half kept, as set_sp does) */
#define F16_PUSH(VAL) do { \
	u8 *_m; \
	uword _sp = (REGi(4) - 2) & spm; \
	FX_MEM(_m, cpu->seg[SEG_SS].base + _sp, 2, 1); \
	*(u16u *)_m = (VAL); \
	REGi(4) = (REGi(4) & ~spm) | _sp; \
} while (0)
#define F16_POP(VAR) do { \
	u8 *_m; \
	uword _sp = REGi(4) & spm; \
	FX_MEM(_m, cpu->seg[SEG_SS].base + _sp, 2, 0); \
	VAR = *(u16u *)_m; \
	REGi(4) = (REGi(4) & ~spm) | ((_sp + 2) & spm); \
} while (0)

/* a real-mode segment load (set_seg's real-mode half; not CS, not SS here) */
#define F16_LOADSEG(S, SEL) do { \
	uword _sel = (SEL) & 0xffff; \
	cpu->seg[S].sel = _sel; \
	cpu->seg[S].base = _sel << 4; \
	cpu->seg[S].limit = 0xffff; \
	cpu->seg[S].flags = 0; \
} while (0)

/* IP: host pointer, fb its base (IP = pc - fb, 16 bits) */
#define F16_IP(ptr) ((uword)((uintptr_t)(ptr) - fb))
#define F16_GO(newip) do { \
	pc = fb + ((newip) & 0xffff); \
	goto loop; \
} while (0)
/* CS changed (far transfers): a new code page from scratch */
#define F16_FAR(newcs, newip) do { \
	F16_LOADSEG(SEG_CS, newcs); \
	csb = cpu->seg[SEG_CS].base; \
	pc = (newip) & 0xffff; \
	fb = 0; \
	wlo = 1; \
	wlen = 0; \
	wend = 0; \
	goto loop; \
} while (0)

#define F16_NEXT do { \
	pc = (uintptr_t)p; \
	if (unlikely(pc >= wend) || unlikely(stepcount <= 0)) \
		goto loop; \
	stepcount--; \
	ovr = -1; \
	b = *p++; \
	goto *tab[b]; \
} while (0)

/* ALU on 16 bits: FX_ALU's, the result stored as a word */
#define F16_ALU(OP, PTR, Y) do { \
	uword _r; \
	FX_ALU(16, OP, *(u16u *)(PTR), (Y), _r); \
	if (OP != 7) \
		*(u16u *)(PTR) = _r; \
} while (0)

#define F16_ALU_OPS(OP) \
	op16_alu_ ## OP ## _0: { /* Eb,Gb */ \
		u8 *ep; \
		u8 *rp = fx_r8(cpu, REG_FIELD(*p)); \
		F16_E(ep, R8P, 1, OP != 7); \
		uword r; \
		FX_ALU(8, OP, *ep, *rp, r); \
		if (OP != 7) *ep = r; \
		F16_NEXT; \
	} \
	op16_alu_ ## OP ## _1: { /* Ew,Gw */ \
		u8 *ep; \
		u16 *rp = R16P(REG_FIELD(*p)); \
		F16_E(ep, R16P, 2, OP != 7); \
		F16_ALU(OP, ep, *rp); \
		F16_NEXT; \
	} \
	op16_alu_ ## OP ## _2: { /* Gb,Eb */ \
		u8 *ep; \
		u8 *rp = fx_r8(cpu, REG_FIELD(*p)); \
		F16_E(ep, R8P, 1, 0); \
		uword r; \
		FX_ALU(8, OP, *rp, *ep, r); \
		if (OP != 7) *rp = r; \
		F16_NEXT; \
	} \
	op16_alu_ ## OP ## _3: { /* Gw,Ew */ \
		u8 *ep; \
		u16 *rp = R16P(REG_FIELD(*p)); \
		F16_E(ep, R16P, 2, 0); \
		F16_ALU(OP, rp, *(u16u *)ep); \
		F16_NEXT; \
	} \
	op16_alu_ ## OP ## _4: { /* AL,Ib */ \
		uword r; \
		FX_ALU(8, OP, cpu->gprx[0].r8[0], *p, r); \
		p++; \
		if (OP != 7) cpu->gprx[0].r8[0] = r; \
		F16_NEXT; \
	} \
	op16_alu_ ## OP ## _5: { /* AX,Iw */ \
		F16_ALU(OP, R16P(0), *(u16u *)p); \
		p += 2; \
		F16_NEXT; \
	}

/* (with the overlay: its third variant, boards/memmap_nx_overlay.ld) */
#if EXEC1_OVERLAY
#define FX16_ATTR __attribute__((section(".ovl_fx16.fast_exec16"), noinline))
#define FX16_TAB_ATTR __attribute__((section(".ovl_fx16.fx16_tab")))
#else
#define FX16_ATTR IRAM_ATTR_FX
#define FX16_TAB_ATTR FX_TAB_ATTR
#endif
static bool FX16_ATTR fast_exec16(CPUI386 *cpu, int stepcount)
{
#define A(OP) &&op16_alu_ ## OP ## _0, &&op16_alu_ ## OP ## _1, &&op16_alu_ ## OP ## _2, \
	&&op16_alu_ ## OP ## _3, &&op16_alu_ ## OP ## _4, &&op16_alu_ ## OP ## _5
#define F &&fallback
#define F8 F, F, F, F, F, F, F, F
	static const void *const tab[256] FX16_TAB_ATTR = {
/* 00 */	A(0), &&op16_pushseg, &&op16_popseg, A(1), &&op16_pushseg, &&op16_0f,
/* 10 */	A(2), &&op16_pushseg, F, A(3), &&op16_pushseg, &&op16_popseg,
/* 20 */	A(4), &&pfx16_es, F, A(5), &&pfx16_cs, F,
/* 30 */	A(6), &&pfx16_ss, F, A(7), &&pfx16_ds, F,
/* 40 */	&&op16_incdec, &&op16_incdec, &&op16_incdec, &&op16_incdec, &&op16_incdec, &&op16_incdec, &&op16_incdec, &&op16_incdec,
		&&op16_incdec, &&op16_incdec, &&op16_incdec, &&op16_incdec, &&op16_incdec, &&op16_incdec, &&op16_incdec, &&op16_incdec,
/* 50 */	&&op16_push, &&op16_push, &&op16_push, &&op16_push, &&op16_push, &&op16_push, &&op16_push, &&op16_push,
		&&op16_pop, &&op16_pop, &&op16_pop, &&op16_pop, &&op16_pop, &&op16_pop, &&op16_pop, &&op16_pop,
/* 60 */	F, F, F, F, &&pfx16_fs, &&pfx16_gs, F, F, &&op16_push_imm, &&op16_imul_i, &&op16_push_imm, &&op16_imul_i, F, F, F, F,
/* 70 */	&&op16_jcc8, &&op16_jcc8, &&op16_jcc8, &&op16_jcc8, &&op16_jcc8, &&op16_jcc8, &&op16_jcc8, &&op16_jcc8,
		&&op16_jcc8, &&op16_jcc8, &&op16_jcc8, &&op16_jcc8, &&op16_jcc8, &&op16_jcc8, &&op16_jcc8, &&op16_jcc8,
/* 80 */	&&op16_grp1, &&op16_grp1, &&op16_grp1, &&op16_grp1, &&op16_test_b, &&op16_test_w, &&op16_xchg_b, &&op16_xchg_w,
		&&op16_mov_88, &&op16_mov_89, &&op16_mov_8a, &&op16_mov_8b, &&op16_mov_8c, &&op16_lea, &&op16_mov_8e, F,
/* 90 */	&&op16_nop, &&op16_xchg_ax, &&op16_xchg_ax, &&op16_xchg_ax, &&op16_xchg_ax, &&op16_xchg_ax, &&op16_xchg_ax, &&op16_xchg_ax,
		&&op16_cbw, &&op16_cwd, &&op16_callf, F, &&op16_pushf, &&op16_popf, F, F,
/* a0 */	&&op16_moffs, &&op16_moffs, &&op16_moffs, &&op16_moffs, &&op16_str1, &&op16_str1, F, F,
		&&op16_test_al, &&op16_test_ax, &&op16_str1, &&op16_str1, &&op16_str1, &&op16_str1, F, F,
/* b0 */	&&op16_mov_r8i, &&op16_mov_r8i, &&op16_mov_r8i, &&op16_mov_r8i, &&op16_mov_r8i, &&op16_mov_r8i, &&op16_mov_r8i, &&op16_mov_r8i,
		&&op16_mov_r16i, &&op16_mov_r16i, &&op16_mov_r16i, &&op16_mov_r16i, &&op16_mov_r16i, &&op16_mov_r16i, &&op16_mov_r16i, &&op16_mov_r16i,
/* c0 */	&&op16_shift, &&op16_shift, &&op16_ret, &&op16_ret, &&op16_lseg, &&op16_lseg, &&op16_mov_c6, &&op16_mov_c7,
		F, &&op16_leave, &&op16_retf, &&op16_retf, F, F, F, F,
/* d0 */	&&op16_shift, &&op16_shift, &&op16_shift, &&op16_shift, F, F, F, &&op16_xlat, F8,
/* e0 */	&&op16_loop, &&op16_loop, &&op16_loop, &&op16_loop, F, F, F, F,
		&&op16_call, &&op16_jmp16, &&op16_jmpf, &&op16_jmp8, F, F, F, F,
/* f0 */	&&pfx16_lock, F, F, &&pfx16_rep, F, &&op16_cmc, &&op16_grp3, &&op16_grp3,
		&&op16_clc, &&op16_stc, &&op16_cli, F, &&op16_cld, &&op16_std, &&op16_grp4, &&op16_grp5,
	};
#undef A
#undef F
#undef F8
	int steps0, ovr;
	uword spm;              /* (SS's: 0xffff, or all of ESP) */
	int fbatch = 1;         /* (cpu_exec1's instructions a fall back: more while they come dense) */
	uword csb;
	/* pc: the instruction's address in host memory, fb + IP; the code page
	 * (but its last 15 bytes, and not past IP FFF0h) is [wlo, wend) */
	uintptr_t pc, fb, wlo, wlen, wend;
	const u8 *p;
	bool srep;
	u8 b;

again:
	if (!fx16_mode(cpu))
		return EXEC1_CALL(cpu, stepcount);
	spm = cpu->sp_mask;
	csb = cpu->seg[SEG_CS].base;
	pc = cpu->next_ip & 0xffff;
	wlo = 1;
	wlen = 0;
	wend = 0;
	fb = 0;
	steps0 = stepcount;
	goto loop;
loop:
	if (unlikely(stepcount <= 0)) {
		cpu->next_ip = pc - fb;
		cpu->ip = cpu->next_ip;
		cpu->cycle += steps0 - stepcount;
		return true;
	}
	if (unlikely(pc - wlo >= wlen)) {
		uword ip = (pc - fb) & 0xffff;
		uword lin = csb + ip;
		if ((lin & 0xfff) >= 4096 - 15 || ip >= 0x10000 - 15 ||
		    ((lin ^ cpu->ifetch.laddr) >= 4096 && !fx_code_page(cpu, ip, lin))) {
			/* (the page's last bytes, IP's wrap, a page it can't read:
			 * cpu_exec1, one instruction) */
			pc = ip;
			fb = 0;
			stepcount--;
			goto fallback_here;
		}
		fb = (uintptr_t)cpu->phys_mem + (cpu->ifetch.xaddr ^ lin) - (uintptr_t)ip;
		pc = fb + ip;
		wlo = pc - (lin & 0xfff);
		wlen = 4096 - 15;
		wend = wlo + wlen;
		if (wend > fb + 0x10000 - 15)
			wend = fb + 0x10000 - 15;
		wlen = wend - wlo;
	}
	stepcount--;
	p = (const u8 *)pc;
	ovr = -1;
	b = *p++;
	goto *tab[b];

	/* ---- prefixes */
pfx16_es: ovr = SEG_ES; b = *p++; goto *tab[b];
pfx16_cs: ovr = SEG_CS; b = *p++; goto *tab[b];
pfx16_ss: ovr = SEG_SS; b = *p++; goto *tab[b];
pfx16_ds: ovr = SEG_DS; b = *p++; goto *tab[b];
pfx16_fs:               /* (a null FS or GS: cpu_exec1's #GP) */
	if (unlikely(!cpu->seg[SEG_FS].limit))
		goto fallback;
	ovr = SEG_FS; b = *p++; goto *tab[b];
pfx16_gs:
	if (unlikely(!cpu->seg[SEG_GS].limit))
		goto fallback;
	ovr = SEG_GS; b = *p++; goto *tab[b];
pfx16_lock: b = *p++; goto *tab[b];
pfx16_rep:              /* REP MOVS/STOS forward */
	b = *p++;
	if (b != 0xa4 && b != 0xa5 && b != 0xaa && b != 0xab)
		goto fallback;
	srep = true;
	goto op16_str;
op16_str1:
	srep = false;
op16_str: {
	/* page by page, as fastloop.inc's: DI/SI/CX 16 bits, a chunk never
	 * past an offset's wrap; a part done stays in CX/SI/DI for a fall back */
	if (cpu->flags & DF)
		goto fallback;
	unsigned size = b & 1 ? 2 : 1;
	int kind = b < 0xa8 ? 0 : b < 0xac ? 1 : 2;     /* MOVS STOS LODS */
	if (kind == 2 && srep)
		goto fallback;
	u32 cnt = srep ? (REGi(1) & 0xffff) : 1;
	uword esb = cpu->seg[SEG_ES].base, dsb = F16_SEGBASE(SEG_DS);
	while (cnt) {
		uword di = REGi(7) & 0xffff, si = REGi(6) & 0xffff;
		u32 n = cnt;
		u8 *dm = NULL, *sm = NULL;
		if (kind != 2) {
			u32 nd = (0x1000 - ((esb + di) & 0xfff)) / size;
			u32 wd = (0x10000 - di) / size;
			if (nd > wd) nd = wd;
			if (n > nd) n = nd;
			/* (a store into the instruction's own page: cpu_exec1) */
			if ((((esb + di) >> 12) - ((csb + (pc - fb)) >> 12)) <= 1u)
				goto fallback;
		}
		if (kind != 1) {
			u32 ns = (0x1000 - ((dsb + si) & 0xfff)) / size;
			u32 ws = (0x10000 - si) / size;
			if (ns > ws) ns = ws;
			if (n > ns) n = ns;
		}
		if (!n)
			goto fallback;
		if (kind != 2)
			FX_MEM(dm, esb + di, size, 1);
		if (kind != 1)
			FX_MEM(sm, dsb + si, size, 0);
		if (kind == 0) {
			if (size == 2)
				for (u32 i = 0; i < n; i++)
					((u16u *)dm)[i] = ((u16u *)sm)[i];
			else
				for (u32 i = 0; i < n; i++)
					dm[i] = sm[i];
		} else if (kind == 1) {
			if (size == 2) {
				u16 v = REGi(0);
				for (u32 i = 0; i < n; i++)
					((u16u *)dm)[i] = v;
			} else {
				u8 v = REGi(0);
				for (u32 i = 0; i < n; i++)
					dm[i] = v;
			}
		} else if (size == 2)
			*R16P(0) = *(u16u *)sm;
		else
			cpu->gprx[0].r8[0] = *sm;
		if (kind != 1)
			REGi(6) = (REGi(6) & 0xffff0000u) | ((si + n * size) & 0xffff);
		if (kind != 2)
			REGi(7) = (REGi(7) & 0xffff0000u) | ((di + n * size) & 0xffff);
		cnt -= n;
		if (srep)
			REGi(1) = (REGi(1) & 0xffff0000u) | cnt;
	}
	F16_NEXT;
}

	/* ---- ALU */
	F16_ALU_OPS(0)
	F16_ALU_OPS(1)
	F16_ALU_OPS(2)
	F16_ALU_OPS(3)
	F16_ALU_OPS(4)
	F16_ALU_OPS(5)
	F16_ALU_OPS(6)
	F16_ALU_OPS(7)

	/* ---- INC/DEC r16 */
op16_incdec: {
	int cf = fx_CF(cpu);
	u16 *rp = R16P(b & 7);
	cpu->cc.dst = sext16(sext16(*rp) + (b < 0x48 ? 1 : -1));
	cpu->cc.op = b < 0x48 ? CC_INC16 : CC_DEC16;
	SET_BIT(cpu->flags, cf, CF);
	cpu->cc.mask = PF | AF | ZF | SF | OF;
	*rp = cpu->cc.dst;
	F16_NEXT;
}

	/* ---- PUSH/POP r16, segments, imm */
op16_push:
	F16_PUSH(*R16P(b & 7));
	F16_NEXT;
op16_pop: {
	u16 v;
	F16_POP(v);
	*R16P(b & 7) = v;
	F16_NEXT;
}
op16_pushseg:
	F16_PUSH(cpu->seg[b >> 3].sel);
	F16_NEXT;
op16_popseg: {          /* POP ES, POP DS (SS: cpu_exec1, its shadow) */
	F16_PM_FALLBACK();
	u16 v;
	F16_POP(v);
	F16_LOADSEG(b >> 3, v);
	F16_NEXT;
}
op16_imul_i: {          /* 69 6b: IMUL Gw,Ew,imm, as i386.c's IMUL2wI */
	u8 *ep;
	u16 *rp = R16P(REG_FIELD(*p));
	F16_E(ep, R16P, 2, 0);
	cpu->cc.src1 = sext16(*(u16u *)ep);
	if (b == 0x69) {
		cpu->cc.src2 = sext16(*(u16u *)p);
		p += 2;
	} else
		cpu->cc.src2 = sext8(*p++);
	cpu->cc.dst = cpu->cc.src1 * cpu->cc.src2;
	cpu->cc.op = CC_IMUL16;
	cpu->cc.mask = CF | PF | AF | ZF | SF | OF;
	*rp = cpu->cc.dst;
	F16_NEXT;
}
op16_0f: {
	u8 b2 = *p++;
	if (b2 >= 0x80 && b2 <= 0x8f) {         /* Jcc rel16 */
		u16 d = *(u16u *)p;
		p += 2;
		if (fx_cond(cpu, b2 & 15))
			F16_GO(F16_IP(p) + d);
		F16_NEXT;
	}
	if (b2 == 0xff) {                       /* the HLE escape (UD0): the BIOS's and DOS's vectors */
		if (!cpu->hle_trap)
			goto fallback;
		cpu->ip = pc - fb;
		cpu->next_ip = cpu->ip + 2;
		cpu->cycle += steps0 - stepcount;
		if (!cpu->hle_trap(cpu)) {
			cpu->stop = true;       /* (as i386.c's UD0: a native->guest call returned) */
			return true;
		}
		if (cpu->intr && (cpu->flags & IF))
			return true;    /* (an interrupt the native code let in) */
		goto again;
	}
	if (b2 >= 0x90 && b2 <= 0x9f) {         /* SETcc Eb */
		u8 *ep;
		F16_E(ep, R8P, 1, 1);
		*ep = fx_cond(cpu, b2 & 15);
		F16_NEXT;
	}
	if (b2 == 0xa0 || b2 == 0xa8) {         /* PUSH FS/GS */
		F16_PUSH(cpu->seg[b2 == 0xa0 ? SEG_FS : SEG_GS].sel);
		F16_NEXT;
	}
	if (b2 == 0xa1 || b2 == 0xa9) {         /* POP FS/GS */
		F16_PM_FALLBACK();
		u16 v;
		F16_POP(v);
		F16_LOADSEG(b2 == 0xa1 ? SEG_FS : SEG_GS, v);
		F16_NEXT;
	}
	u8 *ep;
	u16 *rp = R16P(REG_FIELD(*p));
	switch (b2) {                           /* MOVZX MOVSX IMUL: Gw,E */
	case 0xb6:
		F16_E(ep, R8P, 1, 0);
		*rp = *ep;
		F16_NEXT;
	case 0xb7:
		F16_E(ep, R16P, 2, 0);
		*rp = *(u16u *)ep;
		F16_NEXT;
	case 0xbe:
		F16_E(ep, R8P, 1, 0);
		*rp = (s8)*ep;
		F16_NEXT;
	case 0xbf:
		F16_E(ep, R16P, 2, 0);
		*rp = *(u16u *)ep;
		F16_NEXT;
	case 0xaf:
		F16_E(ep, R16P, 2, 0);
		cpu->cc.src1 = sext16(*rp);
		cpu->cc.src2 = sext16(*(u16u *)ep);
		cpu->cc.dst = cpu->cc.src1 * cpu->cc.src2;
		cpu->cc.op = CC_IMUL16;
		cpu->cc.mask = CF | PF | AF | ZF | SF | OF;
		*rp = cpu->cc.dst;
		F16_NEXT;
	}
	goto fallback;
}
op16_push_imm: {
	u16 v;
	if (b == 0x68) {
		v = *(u16u *)p;
		p += 2;
	} else
		v = (s8)*p++;
	F16_PUSH(v);
	F16_NEXT;
}

	/* ---- Jcc, JMP, CALL, RET, LOOP, far transfers */
op16_jcc8: {
	s8 d = *p++;
	if (fx_cond(cpu, b & 15))
		F16_GO(F16_IP(p) + d);
	F16_NEXT;
}
op16_jmp8: {
	s8 d = *p++;
	F16_GO(F16_IP(p) + d);
}
op16_jmp16: {
	u16 d = *(u16u *)p;
	p += 2;
	F16_GO(F16_IP(p) + d);
}
op16_call: {
	u16 d = *(u16u *)p;
	p += 2;
	F16_PUSH(F16_IP(p));
	F16_GO(F16_IP(p) + d);
}
op16_ret: {
	u16 ip, n = b == 0xc2 ? *(u16u *)p : 0;
	F16_POP(ip);
	REGi(4) = (REGi(4) & ~spm) | ((REGi(4) + n) & spm);
	F16_GO(ip);
}
op16_leave: {           /* SP = BP, POP BP (as i386.c's: EBP's all, masked) */
	u8 *m;
	uword sp = REGi(5);
	FX_MEM(m, cpu->seg[SEG_SS].base + (sp & spm), 2, 0);
	REGi(4) = (REGi(4) & ~spm) | ((sp + 2) & spm);
	*R16P(5) = *(u16u *)m;
	F16_NEXT;
}
op16_retf: {
	F16_PM_FALLBACK();
	u16 ip, cs, n = b == 0xca ? *(u16u *)p : 0;
	u8 *m;
	uword sp = REGi(4) & spm;
	FX_MEM(m, cpu->seg[SEG_SS].base + sp, 2, 0);
	ip = *(u16u *)m;
	FX_MEM(m, cpu->seg[SEG_SS].base + ((sp + 2) & spm), 2, 0);
	cs = *(u16u *)m;
	REGi(4) = (REGi(4) & ~spm) | ((sp + 4 + n) & spm);
	F16_FAR(cs, ip);
}
op16_callf: {
	F16_PM_FALLBACK();
	u16 ip = *(u16u *)p, cs = *(u16u *)(p + 2);
	p += 4;
	u8 *m1, *m2;
	uword sp = REGi(4) & spm;
	FX_MEM(m1, cpu->seg[SEG_SS].base + ((sp - 2) & spm), 2, 1);
	FX_MEM(m2, cpu->seg[SEG_SS].base + ((sp - 4) & spm), 2, 1);
	*(u16u *)m1 = cpu->seg[SEG_CS].sel;
	*(u16u *)m2 = F16_IP(p);
	REGi(4) = (REGi(4) & ~spm) | ((sp - 4) & spm);
	F16_FAR(cs, ip);
}
op16_jmpf: {
	F16_PM_FALLBACK();
	u16 ip = *(u16u *)p, cs = *(u16u *)(p + 2);
	F16_FAR(cs, ip);
}
op16_loop: {            /* LOOPNE LOOPE LOOP JCXZ, CX (67h: cpu_exec1) */
	s8 d = *p++;
	u16 c = REGi(1);
	int take;
	if (b == 0xe3)
		take = !c;
	else {
		*R16P(1) = --c;
		take = c && (b == 0xe2 || fx_cond(cpu, b == 0xe1 ? 4 : 5));
	}
	if (take)
		F16_GO(F16_IP(p) + d);
	F16_NEXT;
}

	/* ---- group 1: Eb,Ib Ev,Iw Ev,Ib */
op16_grp1: {
	u8 *ep;
	int op = REG_FIELD(*p);
	uword r;
	if (!(b & 1)) {         /* 80, 82 */
		F16_E(ep, R8P, 1, op != 7);
		FX_ALU(8, op, *ep, *p, r);
		p++;
		if (op != 7)
			*ep = r;
	} else {
		F16_E(ep, R16P, 2, op != 7);
		u16 y;
		if (b == 0x83)
			y = (s8)*p++;
		else {
			y = *(u16u *)p;
			p += 2;
		}
		switch (op) {
		case 0: F16_ALU(0, ep, y); break;
		case 1: F16_ALU(1, ep, y); break;
		case 2: F16_ALU(2, ep, y); break;
		case 3: F16_ALU(3, ep, y); break;
		case 4: F16_ALU(4, ep, y); break;
		case 5: F16_ALU(5, ep, y); break;
		case 6: F16_ALU(6, ep, y); break;
		default: F16_ALU(7, ep, y); break;
		}
	}
	F16_NEXT;
}

	/* ---- TEST, XCHG, MOV, LEA, segments */
op16_test_b: {
	u8 *ep;
	u8 *rp = fx_r8(cpu, REG_FIELD(*p));
	F16_E(ep, R8P, 1, 0);
	cpu->cc.dst = sext8(*ep & *rp);
	cpu->cc.op = CC_AND;
	cpu->cc.mask = CF | PF | ZF | SF | OF;
	F16_NEXT;
}
op16_test_w: {
	u8 *ep;
	u16 *rp = R16P(REG_FIELD(*p));
	F16_E(ep, R16P, 2, 0);
	cpu->cc.dst = sext16(*(u16u *)ep & *rp);
	cpu->cc.op = CC_AND;
	cpu->cc.mask = CF | PF | ZF | SF | OF;
	F16_NEXT;
}
op16_xchg_b: {
	u8 *ep;
	u8 *rp = fx_r8(cpu, REG_FIELD(*p));
	F16_E(ep, R8P, 1, 1);
	u8 t = *rp;
	*rp = *ep;
	*ep = t;
	F16_NEXT;
}
op16_xchg_w: {
	u8 *ep;
	u16 *rp = R16P(REG_FIELD(*p));
	F16_E(ep, R16P, 2, 1);
	u16 t = *rp;
	*rp = *(u16u *)ep;
	*(u16u *)ep = t;
	F16_NEXT;
}
op16_xchg_ax: {
	u16 t = *R16P(0);
	*R16P(0) = *R16P(b & 7);
	*R16P(b & 7) = t;
	F16_NEXT;
}
op16_mov_88: {
	u8 *ep;
	u8 *rp = fx_r8(cpu, REG_FIELD(*p));
	F16_E(ep, R8P, 1, 1);
	*ep = *rp;
	F16_NEXT;
}
op16_mov_89: {
	u8 *ep;
	u16 *rp = R16P(REG_FIELD(*p));
	F16_E(ep, R16P, 2, 1);
	*(u16u *)ep = *rp;
	F16_NEXT;
}
op16_mov_8a: {
	u8 *ep;
	u8 *rp = fx_r8(cpu, REG_FIELD(*p));
	F16_E(ep, R8P, 1, 0);
	*rp = *ep;
	F16_NEXT;
}
op16_mov_8b: {
	u8 *ep;
	u16 *rp = R16P(REG_FIELD(*p));
	F16_E(ep, R16P, 2, 0);
	*rp = *(u16u *)ep;
	F16_NEXT;
}
op16_mov_8c: {          /* MOV Ew,Sw */
	u8 *ep;
	int s = REG_FIELD(*p);
	if (s > 5)
		goto fallback;
	F16_E(ep, R16P, 2, 1);
	*(u16u *)ep = cpu->seg[s].sel;
	F16_NEXT;
}
op16_mov_8e: {          /* MOV Sw,Ew (CS: #UD, SS: its shadow: cpu_exec1) */
	F16_PM_FALLBACK();
	u8 *ep;
	int s = REG_FIELD(*p);
	if (s == SEG_CS || s == SEG_SS || s > 5)
		goto fallback;
	F16_E(ep, R16P, 2, 0);
	F16_LOADSEG(s, *(u16u *)ep);
	F16_NEXT;
}
op16_lseg: {            /* LES, LDS */
	F16_PM_FALLBACK();
	u8 mr = *p;
	if ((mr >> 6) == 3)
		goto fallback;
	u8 *m;
	uword ea;
	int ds;
	p = fx16_ea(cpu, p, &ea, &ds);
	FX_MEM(m, F16_SEGBASE(ds) + ea, 4, 0);
	*R16P(REG_FIELD(mr)) = *(u16u *)m;
	F16_LOADSEG(b == 0xc4 ? SEG_ES : SEG_DS, *(u16u *)(m + 2));
	F16_NEXT;
}
op16_lea: {
	u8 mr = *p;
	if ((mr >> 6) == 3)
		goto fallback;
	uword ea;
	int ds;
	p = fx16_ea(cpu, p, &ea, &ds);
	*R16P(REG_FIELD(mr)) = ea;
	F16_NEXT;
}
op16_nop:
	F16_NEXT;
op16_cbw:
	*R16P(0) = (s8)cpu->gprx[0].r8[0];
	F16_NEXT;
op16_cwd:
	*R16P(2) = (s16)*R16P(0) >> 15;
	F16_NEXT;
op16_xlat: {
	u8 *m;
	FX_MEM(m, F16_SEGBASE(SEG_DS) + ((REGi(3) + cpu->gprx[0].r8[0]) & 0xffff), 1, 0);
	cpu->gprx[0].r8[0] = *m;
	F16_NEXT;
}

	/* ---- MOV moffs, TEST acc,imm, MOV r,imm, MOV E,imm */
op16_moffs: {
	u8 *m;
	uword ea = *(u16u *)p;
	p += 2;
	if (b & 1)
		FX_MEM(m, F16_SEGBASE(SEG_DS) + ea, 2, b >= 0xa2);
	else
		FX_MEM(m, F16_SEGBASE(SEG_DS) + ea, 1, b >= 0xa2);
	if (b == 0xa0)
		cpu->gprx[0].r8[0] = *m;
	else if (b == 0xa1)
		*R16P(0) = *(u16u *)m;
	else if (b == 0xa2)
		*m = cpu->gprx[0].r8[0];
	else
		*(u16u *)m = *R16P(0);
	F16_NEXT;
}
op16_test_al:
	cpu->cc.dst = sext8(cpu->gprx[0].r8[0] & *p++);
	cpu->cc.op = CC_AND;
	cpu->cc.mask = CF | PF | ZF | SF | OF;
	F16_NEXT;
op16_test_ax:
	cpu->cc.dst = sext16(*R16P(0) & *(u16u *)p);
	p += 2;
	cpu->cc.op = CC_AND;
	cpu->cc.mask = CF | PF | ZF | SF | OF;
	F16_NEXT;
op16_mov_r8i:
	*fx_r8(cpu, b & 7) = *p++;
	F16_NEXT;
op16_mov_r16i:
	*R16P(b & 7) = *(u16u *)p;
	p += 2;
	F16_NEXT;
op16_mov_c6: {
	u8 *ep;
	if (REG_FIELD(*p))
		goto fallback;
	F16_E(ep, R8P, 1, 1);
	*ep = *p++;
	F16_NEXT;
}
op16_mov_c7: {
	u8 *ep;
	if (REG_FIELD(*p))
		goto fallback;
	F16_E(ep, R16P, 2, 1);
	*(u16u *)ep = *(u16u *)p;
	p += 2;
	F16_NEXT;
}

	/* ---- SHL SHR SAR Eb/Ew by imm8 / 1 / CL (the others: cpu_exec1) */
op16_shift: {
	u8 *ep;
	int op = REG_FIELD(*p);
	if (op == 6)
		goto fallback;
	bool word = b & 1;
	if (word)
		F16_E(ep, R16P, 2, 1);
	else
		F16_E(ep, R8P, 1, 1);
	uword y0 = b <= 0xc1 ? *p++ : b <= 0xd1 ? 1 : cpu->gprx[1].r8[0];
	uword y = y0 & 0x1f;
	int bits = word ? 16 : 8;
	if (op < 4) {
		/* ROL ROR RCL RCR, as i386.c's: CF and OF in flags, out of cc */
		uword x = word ? *(u16u *)ep : *ep;
		uword res = x;
		int cf1, of1;
		if (op < 2) {
			if (!y0)
				F16_NEXT;
			uword r = y0 & (bits - 1);
			if (op == 0) {
				if (r)
					res = word ? sext16((x << r) | (x >> (16 - r))) : sext8((x << r) | (x >> (8 - r)));
				cf1 = res & 1;
				of1 = (res >> (sizeof(uword) * 8 - 1)) ^ cf1;
			} else {
				if (r)
					res = word ? sext16((x >> r) | (x << (16 - r))) : sext8((x >> r) | (x << (8 - r)));
				cf1 = (res >> (bits - 1)) & 1;
				of1 = ((res ^ (res << 1)) >> (bits - 1)) & 1;
			}
		} else {
			uword r = y % (bits + 1);
			if (!r)
				F16_NEXT;
			uword cf = get_CF(cpu);
			if (op == 2) {
				res = (x << r) | (cf << (r - 1)) | (r != 1 ? (x >> (bits + 1 - r)) : 0);
				res = word ? sext16(res) : sext8(res);
				cf1 = (x >> (bits - r)) & 1;
				of1 = (res >> (sizeof(uword) * 8 - 1)) ^ cf1;
			} else {
				res = (x >> r) | (cf << (bits - r)) | (r != 1 ? (x << (bits + 1 - r)) : 0);
				res = word ? sext16(res) : sext8(res);
				cf1 = ((word ? sext16(x << (16 - r)) : sext8(x << (8 - r))) >> (bits - 1)) & 1;
				of1 = ((res ^ (res << 1)) >> (bits - 1)) & 1;
			}
		}
		SET_BIT(cpu->flags, cf1, CF);
		SET_BIT(cpu->flags, of1, OF);
		cpu->cc.mask &= ~(CF | OF);
		if (word)
			*(u16u *)ep = res;
		else
			*ep = res;
		F16_NEXT;
	}
	if (y) {
		if (y > (uword)bits && op == 4)
			goto fallback;  /* (SHL's carry out of range: cpu_exec1's arithmetic) */
		uword x = word ? *(u16u *)ep : *ep;
		if (op == 4) {
			cpu->cc.dst = word ? sext16(x << y) : sext8(x << y);
			cpu->cc.dst2 = (x >> (bits - y)) & 1;
			cpu->cc.op = CC_SHL;
		} else if (op == 5) {
			cpu->cc.src1 = word ? sext16(x) : sext8(x);
			cpu->cc.dst = word ? sext16(x >> y) : sext8(x >> y);
			cpu->cc.dst2 = (x >> (y - 1)) & 1;
			cpu->cc.op = CC_SHR;
		} else {
			sword sx = word ? sext16(x) : sext8(x);
			cpu->cc.dst = sx >> y;
			cpu->cc.dst2 = (sx >> (y - 1)) & 1;
			cpu->cc.op = CC_SAR;
		}
		cpu->cc.mask = CF | PF | ZF | SF | OF;
		if (word)
			*(u16u *)ep = cpu->cc.dst;
		else
			*ep = cpu->cc.dst;
	}
	F16_NEXT;
}

	/* ---- group 3: TEST NOT NEG MUL IMUL DIV IDIV */
op16_grp3: {
	u8 *ep;
	int op = REG_FIELD(*p);
	if (op == 1)
		goto fallback;
	bool word = b & 1;
	if (word)
		F16_E(ep, R16P, 2, op == 2 || op == 3);
	else
		F16_E(ep, R8P, 1, op == 2 || op == 3);
	uword v = word ? *(u16u *)ep : *ep;
	switch (op) {
	case 0:                 /* TEST */
		if (word) {
			cpu->cc.dst = sext16(v & *(u16u *)p);
			p += 2;
		} else
			cpu->cc.dst = sext8(v & *p++);
		cpu->cc.op = CC_AND;
		cpu->cc.mask = CF | PF | ZF | SF | OF;
		break;
	case 2:                 /* NOT */
		if (word)
			*(u16u *)ep = ~v;
		else
			*ep = ~v;
		break;
	case 3:                 /* NEG */
		cpu->cc.src1 = word ? sext16(v) : sext8(v);
		cpu->cc.dst = word ? sext16(-cpu->cc.src1) : sext8(-cpu->cc.src1);
		cpu->cc.op = word ? CC_NEG16 : CC_NEG8;
		cpu->cc.mask = CF | PF | AF | ZF | SF | OF;
		if (word)
			*(u16u *)ep = cpu->cc.dst;
		else
			*ep = cpu->cc.dst;
		break;
	case 4:                 /* MUL */
		if (word) {
			cpu->cc.src1 = *R16P(0);
			cpu->cc.src2 = v;
			cpu->cc.dst = cpu->cc.src1 * cpu->cc.src2;
			cpu->cc.op = CC_MUL16;
			cpu->cc.mask = CF | PF | AF | ZF | SF | OF;
			*R16P(0) = cpu->cc.dst;
			*R16P(2) = cpu->cc.dst >> 16;
		} else {
			cpu->cc.src1 = cpu->gprx[0].r8[0];
			cpu->cc.src2 = v;
			cpu->cc.dst = sext16(cpu->cc.src1 * cpu->cc.src2);
			cpu->cc.op = CC_MUL8;
			cpu->cc.mask = CF | PF | AF | ZF | SF | OF;
			*R16P(0) = cpu->cc.dst;
		}
		break;
	case 5:                 /* IMUL */
		if (word) {
			cpu->cc.src1 = sext16(*R16P(0));
			cpu->cc.src2 = sext16(v);
			cpu->cc.dst = cpu->cc.src1 * cpu->cc.src2;
			cpu->cc.op = CC_IMUL16;
			cpu->cc.mask = CF | PF | AF | ZF | SF | OF;
			*R16P(0) = cpu->cc.dst;
			*R16P(2) = cpu->cc.dst >> 16;
		} else {
			cpu->cc.src1 = sext8(cpu->gprx[0].r8[0]);
			cpu->cc.src2 = sext8(v);
			cpu->cc.dst = cpu->cc.src1 * cpu->cc.src2;
			cpu->cc.op = CC_IMUL8;
			cpu->cc.mask = CF | PF | AF | ZF | SF | OF;
			*R16P(0) = cpu->cc.dst;
		}
		break;
	case 6:                 /* DIV (#DE, the CPU-test quirks: cpu_exec1) */
		if (word) {
			uword src1 = *R16P(0) | (uword)*R16P(2) << 16;
			if (!v || src1 / v > 0xffff || (src1 == 0x5555 && v == 2))
				goto fallback;
			*R16P(0) = src1 / v;
			*R16P(2) = src1 % v;
		} else {
			uword src1 = *R16P(0);
			if (!v || src1 / v > 0xff || (src1 == 5 && v == 2))
				goto fallback;
			cpu->gprx[0].r8[0] = src1 / v;
			cpu->gprx[0].r8[1] = src1 % v;
		}
		break;
	default:                /* IDIV */
		if (word) {
			sword src1 = (s32)(*R16P(0) | (uword)*R16P(2) << 16), src2 = sext16(v);
			if (!src2 || (src1 == INT32_MIN && src2 == -1))
				goto fallback;
			sword res = src1 / src2;
			if (res > 32767 || res < -32768)
				goto fallback;
			*R16P(0) = res;
			*R16P(2) = src1 % src2;
		} else {
			sword src1 = sext16(*R16P(0)), src2 = sext8(v);
			if (!src2)
				goto fallback;
			sword res = src1 / src2;
			if (res > 127 || res < -128)
				goto fallback;
			cpu->gprx[0].r8[0] = res;
			cpu->gprx[0].r8[1] = src1 % src2;
		}
		break;
	}
	F16_NEXT;
}

	/* ---- group 4: INC DEC Eb; group 5: INC DEC CALL JMP PUSH Ew */
op16_grp4: {
	u8 *ep;
	int op = REG_FIELD(*p);
	if (op > 1)
		goto fallback;
	F16_E(ep, R8P, 1, 1);
	int cf = fx_CF(cpu);
	cpu->cc.dst = sext8(sext8(*ep) + (op == 0 ? 1 : -1));
	cpu->cc.op = op == 0 ? CC_INC8 : CC_DEC8;
	SET_BIT(cpu->flags, cf, CF);
	cpu->cc.mask = PF | AF | ZF | SF | OF;
	*ep = cpu->cc.dst;
	F16_NEXT;
}
op16_grp5: {
	u8 *ep;
	int op = REG_FIELD(*p);
	if (op == 7 || ((op == 3 || op == 5) && (*p >> 6) == 3))
		goto fallback;
	if (op == 3 || op == 5) {               /* CALL/JMP FAR Mp: offset, then CS */
		F16_PM_FALLBACK();
		F16_E(ep, R16P, 4, 0);
		u16 nip = *(u16u *)ep, ncs = *(u16u *)(ep + 2);
		if (op == 3) {
			u8 *m1, *m2;
			uword sp = REGi(4) & spm;
			FX_MEM(m1, cpu->seg[SEG_SS].base + ((sp - 2) & spm), 2, 1);
			FX_MEM(m2, cpu->seg[SEG_SS].base + ((sp - 4) & spm), 2, 1);
			*(u16u *)m1 = cpu->seg[SEG_CS].sel;
			*(u16u *)m2 = F16_IP(p);
			REGi(4) = (REGi(4) & ~spm) | ((sp - 4) & spm);
		}
		F16_FAR(ncs, nip);
	}
	F16_E(ep, R16P, 2, op < 2);
	u16 v = *(u16u *)ep;
	if (op < 2) {
		int cf = fx_CF(cpu);
		cpu->cc.dst = sext16(sext16(v) + (op == 0 ? 1 : -1));
		cpu->cc.op = op == 0 ? CC_INC16 : CC_DEC16;
		SET_BIT(cpu->flags, cf, CF);
		cpu->cc.mask = PF | AF | ZF | SF | OF;
		*(u16u *)ep = cpu->cc.dst;
		F16_NEXT;
	}
	if (op == 4)
		F16_GO(v);
	if (op == 2) {
		F16_PUSH(F16_IP(p));
		F16_GO(v);
	}
	F16_PUSH(v);
	F16_NEXT;
}

	/* ---- flags */
op16_clc:
	cpu->cc.mask &= ~CF;
	cpu->flags &= ~CF;
	F16_NEXT;
op16_stc:
	cpu->cc.mask &= ~CF;
	cpu->flags |= CF;
	F16_NEXT;
op16_cmc: {
	int cf = get_CF(cpu);
	cpu->cc.mask &= ~CF;
	SET_BIT(cpu->flags, !cf, CF);
	F16_NEXT;
}
op16_cld:
	cpu->flags &= ~DF;
	F16_NEXT;
op16_cli:               /* (real mode: CPL 0, IOPL no matter) */
	F16_PM_FALLBACK();
	cpu->flags &= ~IF;
	F16_NEXT;
op16_pushf: {
	u8 *m;
	uword sp = (REGi(4) - 2) & spm;
	FX_MEM(m, cpu->seg[SEG_SS].base + sp, 2, 1);
	refresh_flags(cpu);
	cpu->cc.mask = 0;
	*(u16u *)m = cpu->flags;
	REGi(4) = (REGi(4) & ~spm) | sp;
	F16_NEXT;
}
op16_popf: {
	F16_PM_FALLBACK();
	u16 v;
	F16_POP(v);
	cpu->cc.mask = 0;
	cpu->flags = (((cpu->flags & 0xffff0000u) | v) & EFLAGS_MASK) | 0x2;
	if (unlikely((cpu->flags & TF) || (cpu->intr && (cpu->flags & IF)))) {
		/* (a trap, or an interrupt let in: to the caller, after it) */
		cpu->next_ip = F16_IP(p);
		cpu->ip = cpu->next_ip;
		cpu->cycle += steps0 - stepcount;
		return true;
	}
	F16_NEXT;
}
op16_std:
	cpu->flags |= DF;
	F16_NEXT;

fallback:
	/* the instruction from its start, by cpu_exec1 (which counts it) */
fallback_here:
#ifdef FX_STATS
	{
		extern unsigned fx_stats[0x400];
		if (fb && pc - wlo < wlen) {
			const u8 *q = (const u8 *)pc;
			fx_stats[0x380 + (q[0] >> 2)]++;      /* (380h-3BFh: 16-bit, by opcode / 4) */
		} else
			fx_stats[0x37f]++;
	}
#endif
#ifdef FX16_FBSTATS
	{       /* (tools/cpubench: the fall backs by opcode; 1xxh: after 66h/67h) */
		extern unsigned fx16_fb[0x200];
		if (fb && pc - wlo < wlen) {
			const u8 *q = (const u8 *)pc;
			while (*q == 0x26 || *q == 0x2e || *q == 0x36 || *q == 0x3e || *q == 0x64 ||
			       *q == 0x65 || *q == 0xf2 || *q == 0xf3)
				q++;
			fx16_fb[*q == 0x66 || *q == 0x67 ? 0x100 | q[1 + (q[1] == 0x66 || q[1] == 0x67)] : *q]++;
		}
	}
#endif
	cpu->next_ip = pc - fb;
	cpu->cycle += steps0 - stepcount - 1;
	cpu->stop = false;
	{
		/* a fall back after few instructions here (code under 66h/67h,
		 * a run of what this loop lacks): cpu_exec1 takes the next ones
		 * too, up to 32, so the two don't swap at every instruction */
		fbatch = steps0 - stepcount - 1 < 3 ? (fbatch < 8 ? fbatch * 2 : 8) : 1;
		int nb = fbatch <= stepcount + 1 ? fbatch : stepcount + 1;
		/* (it may count more than nb: MOV SS's shadow takes the next) */
		long c1 = cpu->cycle;
		if (!EXEC1_CALL(cpu, nb))
			return false;
		stepcount -= (int)(cpu->cycle - c1) - 1;
	}
	/* (as cpu_exec1's handlers return: HLT, an interrupt let in) */
	if (cpu->halt || cpu->stop || (cpu->intr && (cpu->flags & IF)))
		return true;
	goto again;
}
#undef F16_NEXT
#endif /* FX16_ENABLE */
