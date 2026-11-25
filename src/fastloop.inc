/*
 * The fast loop: the instructions Win32 code is made of, decoded by hand.
 * (included by i386.c, after cpu_exec1)
 *
 * cpu_exec1 is general and keeps its decoding state in memory: about 30
 * instructions of the M33's before an opcode does anything, and a chain of
 * helpers after.  This loop keeps eip and the code page in registers and
 * takes on what an explorer session executes most (MOV, ALU, PUSH/POP,
 * CALL/RET, Jcc, LEA, TEST, MOVZX: 97% of it), in flat 32-bit protected mode
 * with paging, where cpu_exec1's results are its own.
 *
 * Anything else falls back to cpu_exec1 for one instruction: another
 * opcode, a prefix it doesn't take, an instruction near the end of the code
 * page, a memory operand whose TLB entry has no tag for the access (a miss,
 * a page fault to come, the dirty bit to set, memory that isn't plain), a
 * divide.  The fall back happens before anything is changed, so cpu_exec1
 * runs the instruction from its start and raises whatever it raises.
 *
 * The lazy flags are cpu_exec1's (cc.op/src/dst/mask, the same values), so
 * either may follow the other.  tools/cpubench checks the two against the
 * core as it was, instruction sequence by instruction sequence.
 */

/* murm386: where the loop and its kernel live. SRAM cannot hold them and
 * cpu_exec1 both (i386.c: CPU_EXEC1_SRAM); FX_SRAM 1 is frank-react's layout. */
#ifndef FX_SRAM
#define FX_SRAM (!CPU_EXEC1_SRAM)
#endif
/* Run from flash, the loop loses: Doom 1.9's timedemo took 32827 realtics
 * with it and 27759 with cpu_exec1 (in SRAM) alone, on FRANK Next. Its XIP
 * misses share the QSPI bus with the PSRAM the guest's memory is in. So it
 * runs only where it is in SRAM (CPU_EXEC1_SRAM=OFF). */
#ifndef FX16_ENABLE
#define FX16_ENABLE 0           /* frank-dos: fastloop16.inc, the fast loop for 16-bit code */
#endif
/* frank-dos: NX's SRAM overlay, its third variant (jit=2, i386.c): this
 * loop (no kernel) and fastloop16.inc with a compact cpu_exec1, all in
 * SRAM; run only while that variant is loaded (FX16_ON) */
#define FX_OVL (EXEC1_OVERLAY && FX16_ENABLE)
#ifndef FX_ENABLE
#define FX_ENABLE (FX_SRAM || FX_OVL)
#endif
#if FX_OVL
#define IRAM_ATTR_FX __attribute__((section(".ovl_fx16.fx"), noinline))
#define FX_TAB_ATTR __attribute__((section(".ovl_fx16.fx_tab")))
#elif FX_SRAM
#define IRAM_ATTR_FX __not_in_flash()
#define FX_TAB_ATTR __not_in_flash("fx_tab")
#else
#define IRAM_ATTR_FX
#define FX_TAB_ATTR
#endif

typedef u32 __attribute__((aligned(1), may_alias)) u32u;
typedef u16 __attribute__((aligned(1), may_alias)) u16u;

/* ModRM (mod != 3) and what follows it, 32-bit addressing: the offset; p is
 * past it on return (the default segment doesn't matter: DS and SS are flat) */
static inline __attribute__((always_inline))
const u8 *fx_ea(CPUI386 *cpu, const u8 *p, uword *ea)
{
	u8 m = *p++;
	int mod = m >> 6, rm = m & 7;
	uword a;
	if (rm == 4) {
		u8 sib = *p++;
		int b = sib & 7;
		if (b == 5 && mod == 0) {
			a = *(u32u *)p;
			p += 4;
		} else
			a = REGi(b);
		int i = (sib >> 3) & 7;
		if (i != 4)
			a += REGi(i) << (sib >> 6);
	} else if (rm == 5 && mod == 0) {
		a = *(u32u *)p;
		p += 4;
	} else
		a = REGi(rm);
	if (mod == 1)
		a += (s8)*p++;
	else if (mod == 2) {
		a += *(u32u *)p;
		p += 4;
	}
	*ea = a;
	return p;
}

static inline __attribute__((always_inline)) u8 *fx_r8(CPUI386 *cpu, int r)
{
	return (u8 *)&cpu->gprx[r & 3] + (r >> 2);
}

/* the lazy flags of the ALU operations, as cpu_exec1's macros leave them;
 * op: ADD OR ADC SBB AND SUB XOR CMP; returns the result (unwritten for CMP) */
#define FX_ALU(BIT, OPN, x, y, res) do { \
	uword _x = (x), _y = (y); \
	switch (OPN) { \
	case 0: case 5: case 7: \
		cpu->cc.src1 = sext ## BIT(_x); \
		cpu->cc.src2 = sext ## BIT(_y); \
		cpu->cc.dst = sext ## BIT(OPN == 0 ? _x + _y : _x - _y); \
		cpu->cc.op = OPN == 0 ? CC_ADD : CC_SUB; \
		cpu->cc.mask = CF | PF | AF | ZF | SF | OF; \
		break; \
	case 1: case 4: case 6: \
		cpu->cc.dst = sext ## BIT(OPN == 1 ? _x | _y : OPN == 4 ? _x & _y : _x ^ _y); \
		cpu->cc.op = OPN == 1 ? CC_OR : OPN == 4 ? CC_AND : CC_XOR; \
		cpu->cc.mask = CF | PF | ZF | SF | OF; \
		break; \
	default: { \
		int _cf = fx_CF(cpu); \
		cpu->cc.src1 = sext ## BIT(_x); \
		cpu->cc.src2 = sext ## BIT(_y); \
		if (OPN == 2) { \
			cpu->cc.dst = sext ## BIT(cpu->cc.src1 + cpu->cc.src2 + _cf); \
			cpu->cc.op = _cf ? CC_ADC : CC_ADD; \
		} else { \
			cpu->cc.dst = sext ## BIT(cpu->cc.src1 - cpu->cc.src2 - _cf); \
			cpu->cc.op = _cf ? CC_SBB : CC_SUB; \
		} \
		cpu->cc.mask = CF | PF | AF | ZF | SF | OF; \
		break; \
	} \
	} \
	res = cpu->cc.dst; \
} while (0)

/* CF, inline for what sets it most (else get_CF) */
static inline __attribute__((always_inline)) int fx_CF(CPUI386 *cpu)
{
	if (!(cpu->cc.mask & CF))
		return !!(cpu->flags & CF);
	switch (cpu->cc.op) {
	case CC_SUB: return cpu->cc.src1 < cpu->cc.src2;
	case CC_ADD: return cpu->cc.dst < cpu->cc.src2;
	case CC_AND: case CC_OR: case CC_XOR: return 0;
	default: return get_CF(cpu);
	}
}

/* Jcc/SETcc/CMOVcc's condition (cpu_exec1's COND).  After a CMP or SUB, or
 * a TEST or logic op, with all its flags pending, straight from its
 * operands (the same values: src1 src2 dst are sign-extended to 32 bits) */
static inline __attribute__((always_inline)) int fx_cond(CPUI386 *cpu, int c)
{
	int cond;
	uword m = cpu->cc.mask;
	sword s1 = cpu->cc.src1, s2 = cpu->cc.src2, d = cpu->cc.dst;
	if (cpu->cc.op == CC_SUB && m == (CF | PF | AF | ZF | SF | OF)) {
		switch (c >> 1) {
		case 1: cond = (uword)s1 < (uword)s2; break;
		case 2: cond = d == 0; break;
		case 3: cond = (uword)s1 <= (uword)s2; break;
		case 4: cond = d < 0; break;
		case 6: cond = s1 < s2; break;
		case 7: cond = s1 <= s2; break;
		default: goto slow;
		}
		return cond ^ (c & 1);
	}
	if ((cpu->cc.op == CC_AND || cpu->cc.op == CC_OR || cpu->cc.op == CC_XOR) &&
	    m == (CF | PF | ZF | SF | OF)) {
		switch (c >> 1) {
		case 0: case 1: cond = 0; break;
		case 2: case 3: cond = d == 0; break;
		case 4: case 6: cond = d < 0; break;
		case 7: cond = d <= 0; break;
		default: goto slow;
		}
		return cond ^ (c & 1);
	}
slow:
	switch (c >> 1) {
	case 0: cond = get_OF(cpu); break;
	case 1: cond = get_CF(cpu); break;
	case 2: cond = get_ZF(cpu); break;
	case 3: cond = get_ZF(cpu) || get_CF(cpu); break;
	case 4: cond = get_SF(cpu); break;
	case 5: cond = get_PF(cpu); break;
	case 6: cond = get_SF(cpu) != get_OF(cpu); break;
	default: cond = get_ZF(cpu) || get_SF(cpu) != get_OF(cpu); break;
	}
	return cond ^ (c & 1);
}

/* a TLB miss: the entry for la filled (and tagged) the way cpu_exec1's
 * access would, if that succeeds; false for anything else (an access
 * across pages, a fault to raise: the instruction falls back) */
static bool IRAM_ATTR_FX fx_fill(CPUI386 *cpu, uword la, int size, int w)
{
	uword paddr, lpgno = la >> 12;
	if ((la & 0xfff) > 0x1000u - size)
		return false;
	if (!(cpu->cr0 & CR0_PG)) {             /* murm386: no paging, the identity map */
		if (!plain_page(cpu, la))
			return false;
		tlb_tag_identity(cpu, la);
		return true;
	}
	if (!translate_lpgno(cpu, w ? 2 : 1, lpgno, la, cpu->cpl, &paddr))
		return false;
	struct tlb_entry *e = &cpu->tlb.tab[lpgno % tlb_size];
	return (w ? e->wtag : e->rtag) == lpgno;
}

/* the host address of the code page at CS:eip (lin), if the TLB has it or
 * can have it without a fault (nothing is raised: cr2 and excno stay) */
static const u8 *IRAM_ATTR_FX fx_code_ptr(CPUI386 *cpu, uword eip, uword lin)
{
	OptAddr res;
	uword cr2 = cpu->cr2, excerr = cpu->excerr;
	int excno = cpu->excno;
	if (!translate8r(cpu, &res, SEG_CS, eip) || res.res != ADDR_OK1) {
		cpu->cr2 = cr2;
		cpu->excno = excno;
		cpu->excerr = excerr;
		return NULL;
	}
	return cpu->phys_mem + res.addr1;
}

/* the instruction fetch's page to the one at CS:eip (lin), if it is plain
 * memory and the TLB has it or can have it (a fault: cpu_exec1 raises it) */
static bool IRAM_ATTR_FX fx_code_page(CPUI386 *cpu, uword eip, uword lin)
{
	OptAddr res;
	if (!translate8r(cpu, &res, SEG_CS, eip) || res.res != ADDR_OK1)
		return false;
	cpu->ifetch.laddr = lin & (~4095ul);
	cpu->ifetch.xaddr = res.addr1 ^ lin;
	return true;
}

/* can the fast loop run at all (else cpu_exec1 has the slice). murm386:
 * with paging or without (a DOS extender's flat 32-bit mode), see fx_fill;
 * DS, ES and SS on one base (dbase), not only 0: DJGPP's under CWSDPMI */
static inline bool fx_mode(CPUI386 *cpu)
{
	return !cpu->code16 && cpu->sp_mask == 0xffffffff &&
		(cpu->cr0 & 1) && !(cpu->flags & VM) &&
		cpu->seg[SEG_DS].limit && cpu->seg[SEG_ES].limit && cpu->seg[SEG_SS].limit &&
		cpu->seg[SEG_ES].base == cpu->seg[SEG_DS].base && cpu->seg[SEG_SS].base == cpu->seg[SEG_DS].base;
}

/* the memory operand at linear address la: ptr, for an access of SIZE bytes
 * (W: written) that the TLB tag allows; else the instruction falls back.
 * (One compare: an access across the page's end has another page number
 * at its last byte.) */
#define FX_MEM(ptr, la, SIZE, W) do { \
	uword _la = (la); \
	struct tlb_entry *_e = &cpu->tlb.tab[(_la >> 12) % tlb_size]; \
	if (unlikely(((W) ? _e->wtag : _e->rtag) != (uword)(_la + (SIZE) - 1) >> 12) && \
	    !fx_fill(cpu, _la, SIZE, W)) \
		goto fallback; \
	ptr = (u8 *)(_e->hbase + _la); \
} while (0)

/* the E operand of the ModRM at p: a register (REGP(i)) or memory (DS, ES
 * and SS are flat in the fast loop: obase is an override's base, else 0) */
#define FX_E(ptr, REGP, SIZE, W) do { \
	u8 _mr = *p; \
	if ((_mr >> 6) == 3) { \
		ptr = (u8 *)REGP(_mr & 7); \
		p++; \
	} else { \
		uword _ea; \
		p = fx_ea(cpu, p, &_ea); \
		FX_MEM(ptr, obase + _ea, SIZE, W); \
	} \
} while (0)
#define R32P(i) (&REGi(i))
#define R16P(i) (&REGi(i))
#define R8P(i) fx_r8(cpu, i)
#define REG_FIELD(mr) (((mr) >> 3) & 7)

/* ALU on 32 bits: OP constant (ADD OR ADC SBB AND SUB XOR CMP) */
#define FX_ALU32(OP, DST, SRC) do { \
	u32 _x = (DST), _y = (SRC); \
	if (OP == 0 || OP == 5 || OP == 7) { \
		cpu->cc.src1 = _x; \
		cpu->cc.src2 = _y; \
		cpu->cc.dst = OP == 0 ? _x + _y : _x - _y; \
		cpu->cc.op = OP == 0 ? CC_ADD : CC_SUB; \
		cpu->cc.mask = CF | PF | AF | ZF | SF | OF; \
	} else if (OP == 1 || OP == 4 || OP == 6) { \
		cpu->cc.dst = OP == 1 ? _x | _y : OP == 4 ? _x & _y : _x ^ _y; \
		cpu->cc.op = OP == 1 ? CC_OR : OP == 4 ? CC_AND : CC_XOR; \
		cpu->cc.mask = CF | PF | ZF | SF | OF; \
	} else { \
		int _cf = fx_CF(cpu); \
		cpu->cc.src1 = _x; \
		cpu->cc.src2 = _y; \
		if (OP == 2) { \
			cpu->cc.dst = _x + _y + _cf; \
			cpu->cc.op = _cf ? CC_ADC : CC_ADD; \
		} else { \
			cpu->cc.dst = _x - _y - _cf; \
			cpu->cc.op = _cf ? CC_SBB : CC_SUB; \
		} \
		cpu->cc.mask = CF | PF | AF | ZF | SF | OF; \
	} \
	if (OP != 7) \
		DST = cpu->cc.dst; \
} while (0)

/* the 32 ALU opcodes 00-3d of one operation */
#define FX_ALU_OPS(OP) \
	op_alu_ ## OP ## _0: { /* Eb,Gb */ \
		u8 *ep; \
		u8 *rp = R8P(REG_FIELD(*p)); \
		FX_E(ep, R8P, 1, OP != 7); \
		uword r; \
		FX_ALU(8, OP, *ep, *rp, r); \
		if (OP != 7) *ep = r; \
		NEXT; \
	} \
	op_alu_ ## OP ## _1: { /* Ev,Gv */ \
		u8 *ep; \
		u32 *rp = R32P(REG_FIELD(*p)); \
		FX_E(ep, R32P, 4, OP != 7); \
		FX_ALU32(OP, *(u32u *)ep, *rp); \
		NEXT; \
	} \
	op_alu_ ## OP ## _2: { /* Gb,Eb */ \
		u8 *ep; \
		u8 *rp = R8P(REG_FIELD(*p)); \
		FX_E(ep, R8P, 1, 0); \
		uword r; \
		FX_ALU(8, OP, *rp, *ep, r); \
		if (OP != 7) *rp = r; \
		NEXT; \
	} \
	op_alu_ ## OP ## _3: { /* Gv,Ev */ \
		u8 *ep; \
		u32 *rp = R32P(REG_FIELD(*p)); \
		FX_E(ep, R32P, 4, 0); \
		FX_ALU32(OP, *rp, *(u32u *)ep); \
		NEXT; \
	} \
	op_alu_ ## OP ## _4: { /* AL,Ib */ \
		uword r; \
		FX_ALU(8, OP, cpu->gprx[0].r8[0], *p, r); \
		p++; \
		if (OP != 7) cpu->gprx[0].r8[0] = r; \
		NEXT; \
	} \
	op_alu_ ## OP ## _5: { /* eAX,Id */ \
		FX_ALU32(OP, REGi(0), *(u32u *)p); \
		p += 4; \
		NEXT; \
	}

#if FX_KERNEL
/* fastloop_m33.S: the commonest instructions, in Thumb-2 */
#include <stddef.h>
struct fx_state {
	uintptr_t pc, wlo, wend, fb;
	int stepcount;
};
void fx_kernel(CPUI386 *cpu, struct fx_state *st);
extern const uintptr_t fx_ktab[256];    /* its dispatch: fx_kernel_out + 1, not its own */
extern const char fx_kernel_out[];
#ifdef FX_KERNEL_SWITCH
extern int fx_kernel_on;        /* (tools/cpubench: the kernel against the C loop) */
#define FX_KERNEL_ON fx_kernel_on
#else
#define FX_KERNEL_ON 1
#endif
_Static_assert(offsetof(CPUI386, gprx[4]) == 16, "fastloop_m33.S: OFF_ESP");
_Static_assert(offsetof(CPUI386, flags) == 40, "fastloop_m33.S: OFF_FLAGS");
_Static_assert(offsetof(CPUI386, cc.op) == 264, "fastloop_m33.S: OFF_CC_OP");
_Static_assert(offsetof(CPUI386, cc.dst) == 268, "fastloop_m33.S: OFF_CC_DST");
_Static_assert(offsetof(CPUI386, cc.src1) == 276, "fastloop_m33.S: OFF_CC_SRC1");
_Static_assert(offsetof(CPUI386, cc.src2) == 280, "fastloop_m33.S: OFF_CC_SRC2");
_Static_assert(offsetof(CPUI386, cc.mask) == 284, "fastloop_m33.S: OFF_CC_MASK");
_Static_assert(offsetof(CPUI386, tlb.tab) == 292, "fastloop_m33.S: OFF_TLB_TAB");
_Static_assert(offsetof(struct tlb_entry, rtag) == 0 && offsetof(struct tlb_entry, wtag) == 4 &&
	       offsetof(struct tlb_entry, hbase) == 8 && sizeof(struct tlb_entry) == 32, "fastloop_m33.S: TLB");
_Static_assert(tlb_size == 512, "fastloop_m33.S: TLB index");
_Static_assert(CC_ADD == 1 && CC_SUB == 3 && CC_AND == 26 && CC_OR == 27 && CC_XOR == 28, "fastloop_m33.S: CC_");
_Static_assert(offsetof(struct fx_state, stepcount) == 16, "fastloop_m33.S: ST_");

/* the next instruction: back to the top, where the kernel takes it if it can */
#define NEXT do { \
	pc = (uintptr_t)p; \
	goto loop; \
} while (0)
#else
/* the next instruction, straight on: while it's within the code page, no
 * other checks (the slice's count is checked at the taken branches) */
#define NEXT do { \
	pc = (uintptr_t)p; \
	stepcount--; \
	if (unlikely(pc >= wend)) \
		goto next_page; \
	obase = dbase; \
	b = *p++; \
	goto *tab[b]; \
} while (0)
#endif

#if FX16_ENABLE
static bool fast_exec16(CPUI386 *cpu, int stepcount);  /* fastloop16.inc: 16-bit code */
static inline bool fx16_mode(CPUI386 *cpu);
#endif

static bool IRAM_ATTR_FX fast_exec(CPUI386 *cpu, int stepcount)
{
#define A(OP) &&op_alu_ ## OP ## _0, &&op_alu_ ## OP ## _1, &&op_alu_ ## OP ## _2, &&op_alu_ ## OP ## _3, \
	&&op_alu_ ## OP ## _4, &&op_alu_ ## OP ## _5
#define F &&fallback
#define F8 F, F, F, F, F, F, F, F
	static const void *const tab[256] FX_TAB_ATTR = {
/* 00 */	A(0), F, F, A(1), F, &&op_0f,
/* 10 */	A(2), F, F, A(3), F, F,
/* 20 */	A(4), &&pfx_es, F, A(5), &&pfx_cs, F,
/* 30 */	A(6), &&pfx_ss, F, A(7), &&pfx_ds, F,
/* 40 */	&&op_incdec, &&op_incdec, &&op_incdec, &&op_incdec, &&op_incdec, &&op_incdec, &&op_incdec, &&op_incdec,
		&&op_incdec, &&op_incdec, &&op_incdec, &&op_incdec, &&op_incdec, &&op_incdec, &&op_incdec, &&op_incdec,
/* 50 */	&&op_push, &&op_push, &&op_push, &&op_push, &&op_push, &&op_push, &&op_push, &&op_push,
		&&op_pop, &&op_pop, &&op_pop, &&op_pop, &&op_pop, &&op_pop, &&op_pop, &&op_pop,
/* 60 */	F, F, F, F, &&pfx_fs, &&pfx_gs, &&pfx_66, F, &&op_push_imm, &&op_imul_imm, &&op_push_imm, &&op_imul_imm, F, F, F, F,
/* 70 */	&&op_jcc8, &&op_jcc8, &&op_jcc8, &&op_jcc8, &&op_jcc8, &&op_jcc8, &&op_jcc8, &&op_jcc8,
		&&op_jcc8, &&op_jcc8, &&op_jcc8, &&op_jcc8, &&op_jcc8, &&op_jcc8, &&op_jcc8, &&op_jcc8,
/* 80 */	&&op_grp1, &&op_grp1, F, &&op_grp1, &&op_test_b, &&op_test_d, &&op_xchg_b, &&op_xchg_d,
		&&op_mov_88, &&op_mov_89, &&op_mov_8a, &&op_mov_8b, F, &&op_lea, F, F,
/* 90 */	&&op_nop, F, F, F, F, F, F, F, &&op_cwde, &&op_cdq, F, F, F, F, F, F,
/* a0 */	&&op_moffs, &&op_moffs, &&op_moffs, &&op_moffs, &&op_str1, &&op_str1, F, F,
		&&op_test_al, &&op_test_eax, &&op_str1, &&op_str1, F, F, F, F,
/* b0 */	&&op_mov_r8i, &&op_mov_r8i, &&op_mov_r8i, &&op_mov_r8i, &&op_mov_r8i, &&op_mov_r8i, &&op_mov_r8i, &&op_mov_r8i,
		&&op_mov_r32i, &&op_mov_r32i, &&op_mov_r32i, &&op_mov_r32i, &&op_mov_r32i, &&op_mov_r32i, &&op_mov_r32i, &&op_mov_r32i,
/* c0 */	F, &&op_shift, &&op_ret, &&op_ret, F, F, &&op_mov_c6, &&op_mov_c7, F, &&op_leave, F, F, F, F, F, F,
/* d0 */	F, &&op_shift, F, &&op_shift, F, F, F, F,
		&&op_esc, &&op_esc, &&op_esc, &&op_esc, &&op_esc, &&op_esc, &&op_esc, &&op_esc,
/* e0 */	&&op_loop, &&op_loop, &&op_loop, &&op_loop, F, F, F, F, &&op_call, &&op_jmp32, F, &&op_jmp8, F, F, F, F,
/* f0 */	&&pfx_lock, F, F, &&pfx_rep, F, F, &&op_grp3, &&op_grp3, F, F, F, F, F, F, F, &&op_grp5,
	};
	/* after 0x66: what the operand size doesn't change, and some of what it does */
	static const void *const tab16[256] FX_TAB_ATTR = {
/* 00 */	&&op_alu_0_0, &&w_alu_evgv, &&op_alu_0_2, &&w_alu_evgv, &&op_alu_0_4, &&w_alu_axiw, F, F,
		&&op_alu_1_0, &&w_alu_evgv, &&op_alu_1_2, &&w_alu_evgv, &&op_alu_1_4, &&w_alu_axiw, F, &&w_0f,
/* 10 */	F8, F8,
/* 20 */	&&op_alu_4_0, &&w_alu_evgv, &&op_alu_4_2, &&w_alu_evgv, &&op_alu_4_4, &&w_alu_axiw, &&w_pfx_es, F,
		&&op_alu_5_0, &&w_alu_evgv, &&op_alu_5_2, &&w_alu_evgv, &&op_alu_5_4, &&w_alu_axiw, &&w_pfx_cs, F,
/* 30 */	&&op_alu_6_0, &&w_alu_evgv, &&op_alu_6_2, &&w_alu_evgv, &&op_alu_6_4, &&w_alu_axiw, &&w_pfx_ss, F,
		&&op_alu_7_0, &&w_alu_evgv, &&op_alu_7_2, &&w_alu_evgv, &&op_alu_7_4, &&w_alu_axiw, &&w_pfx_ds, F,
/* 40 */	F8, F8,
/* 50 */	F8, F8,
/* 60 */	F, F, F, F, &&w_pfx_fs, &&w_pfx_gs, F, F, F8,
/* 70 */	&&op_jcc8, &&op_jcc8, &&op_jcc8, &&op_jcc8, &&op_jcc8, &&op_jcc8, &&op_jcc8, &&op_jcc8,
		&&op_jcc8, &&op_jcc8, &&op_jcc8, &&op_jcc8, &&op_jcc8, &&op_jcc8, &&op_jcc8, &&op_jcc8,
/* 80 */	&&op_grp1, &&w_grp1, F, &&w_grp1, &&op_test_b, &&w_test, F, F,
		&&op_mov_88, &&w_mov_89, &&op_mov_8a, &&w_mov_8b, F, F, F, F,
/* 90 */	&&op_nop, F, F, F, F, F, F, F, F8,
/* a0 */	F, &&w_moffs, F, &&w_moffs, F, F, F, F, F, &&w_test_ax, F, F, F, F, F, F,
/* b0 */	&&op_mov_r8i, &&op_mov_r8i, &&op_mov_r8i, &&op_mov_r8i, &&op_mov_r8i, &&op_mov_r8i, &&op_mov_r8i, &&op_mov_r8i,
		&&w_mov_r16i, &&w_mov_r16i, &&w_mov_r16i, &&w_mov_r16i, &&w_mov_r16i, &&w_mov_r16i, &&w_mov_r16i, &&w_mov_r16i,
/* c0 */	F, &&w_shift, F, F, F, F, &&op_mov_c6, &&w_mov_c7, F8,
/* d0 */	F, &&w_shift, F, &&w_shift, F, F, F, F, F8,
/* e0 */	F8, F, F, F, &&op_jmp8, F, F, F, F,
/* f0 */	&&w_pfx_lock, F, F, F, F, F, F, F, F8,
	};
#undef A
#undef F
#undef F8
	int steps0;
	uword csb;
	/* pc: the instruction's address in host memory, fb + eip; the code page
	 * (but its last 15 bytes) is [wlo, wlo + wlen) */
	uintptr_t pc, fb, wlo, wlen, wend;
	const u8 *p;
	uword obase;            /* a segment override's base, else dbase */
	uword dbase;            /* DS's, ES's and SS's (fx_mode) */
	u8 ibuf[16];            /* an instruction across two pages */
	bool srep;              /* a string op's REP */
	u8 b;

again:
	if (!FX_ENABLE || !fx_mode(cpu)) {
#ifdef FX_STATS
		extern unsigned fx_stats[0x400];
		fx_stats[0x3ff] += stepcount;   /* (a slice not for it: fx_mode) */
		/* which condition (3F0h+): code16, sp_mask, real mode, VM, a limit 0, ES or SS off DS */
		fx_stats[cpu->code16 ? ((cpu->cr0 & 1) ? 0x3f7 : 0x3f0) : cpu->sp_mask != 0xffffffff ? 0x3f1 : !(cpu->cr0 & 1) ? 0x3f2 :
			 (cpu->flags & VM) ? 0x3f3 :
			 !(cpu->seg[SEG_DS].limit && cpu->seg[SEG_ES].limit && cpu->seg[SEG_SS].limit) ? 0x3f4 :
			 cpu->seg[SEG_ES].base != cpu->seg[SEG_DS].base ? 0x3f5 : 0x3f6] += stepcount;
#endif
#if FX16_ENABLE
#ifdef FX_STATS
		/* (3E0h: slices fastloop16.inc takes; 3E1h-3E4h refused in protected
		 * mode: a 32-bit stack, VM, TF, a null DS/ES/SS) */
		if (cpu->code16 && (cpu->cr0 & 1))
			fx_stats[FX16_ON && fx16_mode(cpu) ? 0x3e0 : cpu->sp_mask != 0xffff ? 0x3e1 :
				 (cpu->flags & VM) ? 0x3e2 : (cpu->flags & TF) ? 0x3e3 : 0x3e4] += stepcount;
#endif
		if (FX16_ON && fx16_mode(cpu))
			return fast_exec16(cpu, stepcount);
#endif
		return EXEC1_CALL(cpu, stepcount);
	}
	pc = cpu->next_ip;
	csb = cpu->seg[SEG_CS].base;
	dbase = cpu->seg[SEG_DS].base;
	wlo = 1;
	wlen = 0;
	wend = 0;
	fb = 0;
	steps0 = stepcount;
	goto loop;
next_page:
	stepcount++;
loop:
	if (unlikely(stepcount <= 0)) {
		cpu->next_ip = pc - fb;
		cpu->ip = cpu->next_ip;
		cpu->cycle += steps0 - stepcount;
		return true;
	}
	if (unlikely(pc - wlo >= wlen)) {
		/* the code page, from the instruction fetch's (its last 15
		 * bytes: cpu_exec1, an instruction may cross into the next) */
		u32 eip = pc - fb;
		uword lin = csb + eip;
		if ((lin ^ cpu->ifetch.laddr) >= 4096 && !fx_code_page(cpu, eip, lin)) {
			pc = eip;
			fb = 0;
			stepcount--;
			goto fallback_here;
		}
		fb = (uintptr_t)cpu->phys_mem + (cpu->ifetch.xaddr ^ lin) - (uintptr_t)eip;
		pc = fb + eip;
		wlo = pc - (lin & 0xfff);
		wlen = 4096 - 15;
		wend = wlo + wlen;
		if (unlikely((lin & 0xfff) >= 4096 - 15)) {
			/* an instruction may cross into the next page: its bytes from
			 * both, in ibuf (one instruction; the next comes back here) */
			unsigned n = 4096 - (lin & 0xfff);
			const u8 *q = fx_code_ptr(cpu, eip + n, (lin | 0xfff) + 1);
			if (!q) {
				pc = eip;
				fb = 0;
				stepcount--;
				goto fallback_here;
			}
			memcpy(ibuf, (const u8 *)pc, n);
			memcpy(ibuf + n, q, 16 - n);
			fb = (uintptr_t)ibuf - eip;
			pc = (uintptr_t)ibuf;
			wlo = pc;
			wlen = 0;
			wend = pc;
		}
	}
#if FX_KERNEL
	if (likely(pc - wlo < wlen) && FX_KERNEL_ON && !dbase &&
	    fx_ktab[*(const u8 *)pc] != (uintptr_t)fx_kernel_out + 1) {
		struct fx_state st = { pc, wlo, wend, fb, stepcount };
		fx_kernel(cpu, &st);
		pc = st.pc;
		stepcount = st.stepcount;
#ifdef FX_KSTATS
		/* (tools/cpubench: what the kernel left to this loop) */
		if (pc - wlo < wlen && stepcount > 0) {
			extern unsigned fx_kstats[0x200];
			const u8 *q = (const u8 *)pc;
			fx_kstats[q[0] == 0x0f ? 0x100 | q[1] : q[0]]++;
		}
#endif
		if (stepcount <= 0 || pc - wlo >= wlen)
			goto loop;
	}
#endif
	stepcount--;
	p = (const u8 *)pc;
	obase = dbase;
	b = *p++;
	goto *tab[b];

	/* ---- prefixes */
pfx_es:
pfx_ss:
pfx_ds:
	b = *p++;
	goto *tab[b];
pfx_cs: obase = csb; goto pfx_seg;
pfx_fs: obase = cpu->seg[SEG_FS].base; if (!cpu->seg[SEG_FS].limit) goto fallback; goto pfx_seg;
pfx_gs: obase = cpu->seg[SEG_GS].base; if (!cpu->seg[SEG_GS].limit) goto fallback;
pfx_seg:
	b = *p++;
	goto *tab[b];
pfx_66:
	b = *p++;
	goto *tab16[b];
pfx_lock:               /* (one CPU: nothing to lock) */
	b = *p++;
	goto *tab[b];
w_pfx_lock:
	b = *p++;
	goto *tab16[b];
pfx_rep:                /* REP STOS/MOVS forward; REP RET */
	b = *p++;
	if (b == 0xc3)
		goto op_ret;
	if (b != 0xa4 && b != 0xa5 && b != 0xaa && b != 0xab)
		goto fallback;
	srep = true;
	goto op_str;
op_str1:
	srep = false;
op_str: {
	/* page by page, the TLB's tags checked once for each: what cpu_exec1's
	 * loops do element by element.  A part done is in ECX/ESI/EDI, so a
	 * fall back (a miss, an element across pages) goes on from there. */
	if (cpu->flags & DF)
		goto fallback;
	unsigned size = b & 1 ? 4 : 1;
	bool movs = b < 0xa8;
	u32 cnt = srep ? REGi(1) : 1;
	while (cnt) {
		uword di = REGi(7);
		u32 n = (0x1000 - ((dbase + di) & 0xfff)) / size;
		u8 *dm, *sm;
		if (n > cnt)
			n = cnt;
		if (!n)
			goto fallback;
		/* murm386: a store into the instruction's own page goes to cpu_exec1,
		 * which runs the REP to its end as decoded; a fall back after it here
		 * would decode the bytes the copy left (paging off, code is writable) */
		if ((((dbase + di) >> 12) - ((csb + (pc - fb)) >> 12)) <= 1u)
			goto fallback;
		FX_MEM(dm, dbase + di, size, 1);
		if (movs) {
			uword si = REGi(6);
			u32 ns = (0x1000 - ((obase + si) & 0xfff)) / size;
			if (!ns)
				goto fallback;
			if (n > ns)
				n = ns;
			FX_MEM(sm, obase + si, size, 0);
			if (size == 4)
				for (u32 i = 0; i < n; i++)
					((u32u *)dm)[i] = ((u32u *)sm)[i];
			else
				for (u32 i = 0; i < n; i++)
					dm[i] = sm[i];
			REGi(6) = si + n * size;
		} else if (size == 4) {
			u32 v = REGi(0);
			for (u32 i = 0; i < n; i++)
				((u32u *)dm)[i] = v;
		} else {
			u8 v = REGi(0);
			for (u32 i = 0; i < n; i++)
				dm[i] = v;
		}
		REGi(7) = di + n * size;
		cnt -= n;
		if (srep)
			REGi(1) = cnt;
	}
	NEXT;
}
w_pfx_es:
w_pfx_ss:
w_pfx_ds:
	b = *p++;
	goto *tab16[b];
w_pfx_cs: obase = csb; goto w_pfx_seg;
w_pfx_fs: obase = cpu->seg[SEG_FS].base; if (!cpu->seg[SEG_FS].limit) goto fallback; goto w_pfx_seg;
w_pfx_gs: obase = cpu->seg[SEG_GS].base; if (!cpu->seg[SEG_GS].limit) goto fallback;
w_pfx_seg:
	b = *p++;
	goto *tab16[b];

	/* ---- ALU */
	FX_ALU_OPS(0)
	FX_ALU_OPS(1)
	FX_ALU_OPS(2)
	FX_ALU_OPS(3)
	FX_ALU_OPS(4)
	FX_ALU_OPS(5)
	FX_ALU_OPS(6)
	FX_ALU_OPS(7)

	/* ---- INC/DEC r32 */
op_incdec: {
	int cf = fx_CF(cpu);
	u32 *rp = &REGi(b & 7);
	if (b < 0x48) {
		cpu->cc.dst = *rp + 1;
		cpu->cc.op = CC_INC32;
	} else {
		cpu->cc.dst = *rp - 1;
		cpu->cc.op = CC_DEC32;
	}
	SET_BIT(cpu->flags, cf, CF);
	cpu->cc.mask = PF | AF | ZF | SF | OF;
	*rp = cpu->cc.dst;
	NEXT;
}

	/* ---- PUSH/POP r32, PUSH imm, IMUL Gv,Ev,imm */
op_push: {
	u8 *m;
	uword sp = REGi(4);
	FX_MEM(m, dbase + sp - 4, 4, 1);
	*(u32u *)m = REGi(b & 7);
	REGi(4) = sp - 4;
	NEXT;
}
op_pop: {
	u8 *m;
	uword sp = REGi(4);
	FX_MEM(m, dbase + sp, 4, 0);
	u32 v = *(u32u *)m;
	REGi(4) = sp + 4;
	REGi(b & 7) = v;
	NEXT;
}
op_push_imm: {
	u8 *m;
	u32 v;
	if (b == 0x68) {
		v = *(u32u *)p;
		p += 4;
	} else
		v = (s8)*p++;
	uword sp = REGi(4);
	FX_MEM(m, dbase + sp - 4, 4, 1);
	*(u32u *)m = v;
	REGi(4) = sp - 4;
	NEXT;
}
op_imul_imm: {
	u8 *ep;
	u8 mr = *p;
	FX_E(ep, R32P, 4, 0);
	u32 x = *(u32u *)ep, y;
	if (b == 0x69) {
		y = *(u32u *)p;
		p += 4;
	} else
		y = (s8)*p++;
	int64_t res = (int64_t)(s32)x * (int64_t)(s32)y;
	cpu->cc.src1 = x;
	cpu->cc.src2 = y;
	cpu->cc.dst = res;
	cpu->cc.dst2 = res >> 32;
	cpu->cc.op = CC_IMUL32;
	cpu->cc.mask = CF | PF | AF | ZF | SF | OF;
	REGi(REG_FIELD(mr)) = cpu->cc.dst;
	NEXT;
}

	/* ---- Jcc, JMP, CALL, RET, LEAVE */
op_jcc8: {
	s8 d = *p++;
	if (fx_cond(cpu, b & 15)) {
		pc = (uintptr_t)p + d;
		goto loop;
	}
	NEXT;
}
op_jmp8: {
	s8 d = *p++;
	pc = (uintptr_t)p + d;
	goto loop;
}
op_jmp32: {
	s32 d = *(u32u *)p;
	pc = (uintptr_t)p + 4 + d;
	goto loop;
}
op_loop: {
	/* murm386: LOOPNE LOOPE LOOP JECXZ, 32-bit ECX (a 0x67 prefix falls
	 * back); no flags change. Win95's delay loops are LOOP: one fall back
	 * each round trip, 96% of all of them, before these were here. */
	s8 d = *p++;
	u32 c = REGi(1);
	int take;
	if (b == 0xe3)
		take = !c;
	else {
		REGi(1) = --c;
		take = c && (b == 0xe2 || fx_cond(cpu, b == 0xe1 ? 4 : 5));
	}
	if (take) {
		pc = (uintptr_t)p + d;
		goto loop;
	}
	NEXT;
}
op_call: {
	u8 *m;
	s32 d = *(u32u *)p;
	p += 4;
	uword sp = REGi(4);
	FX_MEM(m, dbase + sp - 4, 4, 1);
	*(u32u *)m = (uintptr_t)p - fb;
	REGi(4) = sp - 4;
	pc = (uintptr_t)p + d;
	goto loop;
}
op_ret: {
	u8 *m;
	uword sp = REGi(4);
	FX_MEM(m, dbase + sp, 4, 0);
	u32 n = b == 0xc2 ? *(u16u *)p : 0;
	REGi(4) = sp + 4 + n;
	pc = fb + *(u32u *)m;
	goto loop;
}
op_leave: {
	u8 *m;
	uword sp = REGi(5);
	FX_MEM(m, dbase + sp, 4, 0);
	REGi(4) = sp + 4;
	REGi(5) = *(u32u *)m;
	NEXT;
}

	/* ---- group 1: Eb,Ib Ev,Id Ev,Ib */
op_grp1: {
	u8 *ep;
	int op = REG_FIELD(*p);
	uword r;
	if (b == 0x80) {
		FX_E(ep, R8P, 1, op != 7);
		FX_ALU(8, op, *ep, *p, r);
		p++;
		if (op != 7)
			*ep = r;
	} else {
		FX_E(ep, R32P, 4, op != 7);
		u32 y;
		if (b == 0x83)
			y = (s8)*p++;
		else {
			y = *(u32u *)p;
			p += 4;
		}
		switch (op) {
		case 0: FX_ALU32(0, *(u32u *)ep, y); break;
		case 1: FX_ALU32(1, *(u32u *)ep, y); break;
		case 2: FX_ALU32(2, *(u32u *)ep, y); break;
		case 3: FX_ALU32(3, *(u32u *)ep, y); break;
		case 4: FX_ALU32(4, *(u32u *)ep, y); break;
		case 5: FX_ALU32(5, *(u32u *)ep, y); break;
		case 6: FX_ALU32(6, *(u32u *)ep, y); break;
		default: FX_ALU32(7, *(u32u *)ep, y); break;
		}
	}
	NEXT;
}

	/* ---- TEST, XCHG, MOV, LEA */
op_test_b: {
	u8 *ep;
	u8 *rp = R8P(REG_FIELD(*p));
	FX_E(ep, R8P, 1, 0);
	cpu->cc.dst = sext8(*ep & *rp);
	cpu->cc.op = CC_AND;
	cpu->cc.mask = CF | PF | ZF | SF | OF;
	NEXT;
}
op_test_d: {
	u8 *ep;
	u32 *rp = R32P(REG_FIELD(*p));
	FX_E(ep, R32P, 4, 0);
	cpu->cc.dst = *(u32u *)ep & *rp;
	cpu->cc.op = CC_AND;
	cpu->cc.mask = CF | PF | ZF | SF | OF;
	NEXT;
}
op_xchg_b: {
	u8 *ep;
	u8 *rp = R8P(REG_FIELD(*p));
	FX_E(ep, R8P, 1, 1);
	u8 t = *rp;
	*rp = *ep;
	*ep = t;
	NEXT;
}
op_xchg_d: {
	u8 *ep;
	u32 *rp = R32P(REG_FIELD(*p));
	FX_E(ep, R32P, 4, 1);
	u32 t = *rp;
	*rp = *(u32u *)ep;
	*(u32u *)ep = t;
	NEXT;
}
op_mov_88: {
	u8 *ep;
	u8 *rp = R8P(REG_FIELD(*p));
	FX_E(ep, R8P, 1, 1);
	*ep = *rp;
	NEXT;
}
op_mov_89: {
	u8 *ep;
	u32 *rp = R32P(REG_FIELD(*p));
	FX_E(ep, R32P, 4, 1);
	*(u32u *)ep = *rp;
	NEXT;
}
op_mov_8a: {
	u8 *ep;
	u8 *rp = R8P(REG_FIELD(*p));
	FX_E(ep, R8P, 1, 0);
	*rp = *ep;
	NEXT;
}
op_mov_8b: {
	u8 *ep;
	u32 *rp = R32P(REG_FIELD(*p));
	FX_E(ep, R32P, 4, 0);
	*rp = *(u32u *)ep;
	NEXT;
}
op_lea: {
	u8 mr = *p;
	if ((mr >> 6) == 3)
		goto fallback;
	uword ea;
	p = fx_ea(cpu, p, &ea);
	REGi(REG_FIELD(mr)) = ea;
	NEXT;
}
op_nop:
	NEXT;
op_cwde:
	REGi(0) = sext16(cpu->gprx[0].r16);
	NEXT;
op_cdq:
	REGi(2) = (s32)REGi(0) >> 31;
	NEXT;

	/* ---- MOV moffs, TEST acc,imm, MOV r,imm */
op_moffs: {
	u8 *m;
	uword ea = *(u32u *)p;
	p += 4;
	if (b & 1)
		FX_MEM(m, obase + ea, 4, b >= 0xa2);
	else
		FX_MEM(m, obase + ea, 1, b >= 0xa2);
	if (b == 0xa0)
		cpu->gprx[0].r8[0] = *m;
	else if (b == 0xa1)
		REGi(0) = *(u32u *)m;
	else if (b == 0xa2)
		*m = cpu->gprx[0].r8[0];
	else
		*(u32u *)m = REGi(0);
	NEXT;
}
op_test_al:
	cpu->cc.dst = sext8(cpu->gprx[0].r8[0] & *p++);
	cpu->cc.op = CC_AND;
	cpu->cc.mask = CF | PF | ZF | SF | OF;
	NEXT;
op_test_eax:
	cpu->cc.dst = REGi(0) & *(u32u *)p;
	p += 4;
	cpu->cc.op = CC_AND;
	cpu->cc.mask = CF | PF | ZF | SF | OF;
	NEXT;
op_mov_r8i:
	*R8P(b & 7) = *p++;
	NEXT;
op_mov_r32i:
	REGi(b & 7) = *(u32u *)p;
	p += 4;
	NEXT;

	/* ---- SHL SHR SAR Ev by imm8 / 1 / CL (the others: cpu_exec1) */
op_shift: {
	u8 *ep;
	int op = REG_FIELD(*p);
	if (op != 4 && op != 5 && op != 7)
		goto fallback;
	FX_E(ep, R32P, 4, 1);
	uword y = b == 0xc1 ? *p++ : b == 0xd1 ? 1 : cpu->gprx[1].r8[0];
	y &= 0x1f;
	if (y) {
		uword x = *(u32u *)ep;
		if (op == 4) {
			cpu->cc.dst = x << y;
			cpu->cc.dst2 = (x >> (32 - y)) & 1;
			cpu->cc.op = CC_SHL;
		} else if (op == 5) {
			cpu->cc.src1 = x;
			cpu->cc.dst = x >> y;
			cpu->cc.dst2 = (x >> (y - 1)) & 1;
			cpu->cc.op = CC_SHR;
		} else {
			cpu->cc.dst = (sword)x >> y;
			cpu->cc.dst2 = ((sword)x >> (y - 1)) & 1;
			cpu->cc.op = CC_SAR;
		}
		cpu->cc.mask = CF | PF | ZF | SF | OF;
		*(u32u *)ep = cpu->cc.dst;
	}
	NEXT;
}

	/* ---- MOV Eb,Ib / Ev,Id */
op_mov_c6: {
	u8 *ep;
	if (REG_FIELD(*p))
		goto fallback;
	FX_E(ep, R8P, 1, 1);
	*ep = *p++;
	NEXT;
}
op_mov_c7: {
	u8 *ep;
	if (REG_FIELD(*p))
		goto fallback;
	FX_E(ep, R32P, 4, 1);
	*(u32u *)ep = *(u32u *)p;
	p += 4;
	NEXT;
}

	/* ---- group 3: TEST NOT NEG (MUL, DIV: cpu_exec1) */
op_grp3: {
	u8 *ep;
	int op = REG_FIELD(*p);
	if (op >= 4 && b == 0xf7) {
		FX_E(ep, R32P, 4, 0);
		u32 y = *(u32u *)ep;
		if (op == 4) {          /* MUL */
			uint64_t res = (uint64_t)REGi(0) * y;
			cpu->cc.src1 = REGi(0);
			cpu->cc.src2 = y;
			cpu->cc.dst = res;
			cpu->cc.dst2 = res >> 32;
			cpu->cc.op = CC_MUL32;
		} else if (op == 5) {   /* IMUL */
			int64_t res = (int64_t)(s32)REGi(0) * (s32)y;
			cpu->cc.src1 = REGi(0);
			cpu->cc.src2 = y;
			cpu->cc.dst = res;
			cpu->cc.dst2 = (uint64_t)res >> 32;
			cpu->cc.op = CC_IMUL32;
		} else if (op == 6) {   /* DIV (by 0, too big: cpu_exec1 raises #DE) */
			if (!y)
				goto fallback;
			if (!REGi(2)) {
				u32 x = REGi(0);
				REGi(0) = x / y;
				REGi(2) = x % y;
			} else {
				uint64_t x = (uint64_t)REGi(2) << 32 | REGi(0);
				uint64_t q = x / y;
				if (q >> 32)
					goto fallback;
				REGi(0) = q;
				REGi(2) = x % y;
			}
			NEXT;
		} else {                /* IDIV */
			if (!y)
				goto fallback;
			s32 x = REGi(0);
			if ((s32)REGi(2) == x >> 31 && !(x == INT32_MIN && (s32)y == -1)) {
				REGi(0) = x / (s32)y;
				REGi(2) = x % (s32)y;
			} else {
				int64_t x64 = (int64_t)((uint64_t)REGi(2) << 32 | REGi(0));
				int64_t q = x64 / (s32)y;
				if (q > INT32_MAX || q < INT32_MIN)
					goto fallback;
				REGi(0) = q;
				REGi(2) = x64 % (s32)y;
			}
			NEXT;
		}
		cpu->cc.mask = CF | PF | AF | ZF | SF | OF;
		REGi(0) = cpu->cc.dst;
		REGi(2) = cpu->cc.dst2;
		NEXT;
	}
	if (op == 1 || op > 3)
		goto fallback;
	if (b == 0xf6) {
		FX_E(ep, R8P, 1, op != 0);
		if (op == 0) {
			cpu->cc.dst = sext8(*ep & *p++);
			cpu->cc.op = CC_AND;
			cpu->cc.mask = CF | PF | ZF | SF | OF;
		} else if (op == 2)
			*ep = ~*ep;
		else {
			cpu->cc.src1 = sext8(*ep);
			cpu->cc.dst = sext8(-cpu->cc.src1);
			cpu->cc.op = CC_NEG8;
			cpu->cc.mask = CF | PF | AF | ZF | SF | OF;
			*ep = cpu->cc.dst;
		}
	} else {
		FX_E(ep, R32P, 4, op != 0);
		if (op == 0) {
			cpu->cc.dst = *(u32u *)ep & *(u32u *)p;
			p += 4;
			cpu->cc.op = CC_AND;
			cpu->cc.mask = CF | PF | ZF | SF | OF;
		} else if (op == 2)
			*(u32u *)ep = ~*(u32u *)ep;
		else {
			cpu->cc.src1 = *(u32u *)ep;
			cpu->cc.dst = -cpu->cc.src1;
			cpu->cc.op = CC_NEG32;
			cpu->cc.mask = CF | PF | AF | ZF | SF | OF;
			*(u32u *)ep = cpu->cc.dst;
		}
	}
	NEXT;
}

	/* ---- group 5: INC DEC CALL JMP PUSH Ev */
op_grp5: {
	u8 *ep, *m;
	int op = REG_FIELD(*p);
	if (op == 3 || op == 5 || op == 7)
		goto fallback;
	FX_E(ep, R32P, 4, op < 2);
	u32 v = *(u32u *)ep;
	if (op < 2) {
		int cf = fx_CF(cpu);
		cpu->cc.dst = op == 0 ? v + 1 : v - 1;
		cpu->cc.op = op == 0 ? CC_INC32 : CC_DEC32;
		SET_BIT(cpu->flags, cf, CF);
		cpu->cc.mask = PF | AF | ZF | SF | OF;
		*(u32u *)ep = cpu->cc.dst;
		NEXT;
	}
	if (op == 4) {
		pc = fb + v;
		goto loop;
	}
	uword sp = REGi(4);
	FX_MEM(m, dbase + sp - 4, 4, 1);
	REGi(4) = sp - 4;
	if (op == 2) {
		*(u32u *)m = (uintptr_t)p - fb;
		pc = fb + v;
		goto loop;
	}
	*(u32u *)m = v;
	NEXT;
}

	/* ---- x87: fpu.c, called as cpu_exec1's ESC does (no prefixes) */
op_esc: {
	if ((cpu->cr0 & 0xc) || !cpu->fpu || (const u8 *)pc + 1 != p)
		goto fallback;
	u8 mr = *p;
	bool ok;
	cpu->ip = pc - fb;
	if ((mr >> 6) == 3) {
		p++;
		cpu->next_ip = (uintptr_t)p - fb;
		ok = fpu_exec1(cpu->fpu, cpu, b - 0xd8, REG_FIELD(mr), mr & 7);
	} else {
		uword ea;
		p = fx_ea(cpu, p, &ea);
		cpu->next_ip = (uintptr_t)p - fb;
		/* (DS and SS are the same flat segment here) */
		ok = fpu_exec2(cpu->fpu, cpu, false, b - 0xd8, REG_FIELD(mr), SEG_DS, ea);
	}
	if (!ok) {
		cpu->cycle += steps0 - stepcount;
		return false;
	}
	NEXT;
}

	/* ---- two-byte opcodes */
op_0f: {
	u8 b2 = *p++;
	if (b2 >= 0x80 && b2 <= 0x8f) {         /* Jcc rel32 */
		s32 d = *(u32u *)p;
		p += 4;
		if (fx_cond(cpu, b2 & 15)) {
			pc = (uintptr_t)p + d;
			goto loop;
		}
		NEXT;
	}
	if (b2 == 0xff) {                       /* the HLE escape (UD0) */
		if (!cpu->hle_trap)
			goto fallback;
		cpu->ip = pc - fb;
		cpu->next_ip = cpu->ip + 2;
		cpu->cycle += steps0 - stepcount;
		if (!cpu->hle_trap(cpu)) {
			cpu->stop = true;       /* (frank-dos: as i386.c's UD0, a native->guest call returned) */
			return true;
		}
		goto again;
	}
	if (b2 >= 0x90 && b2 <= 0x9f) {         /* SETcc Eb */
		u8 *ep;
		FX_E(ep, R8P, 1, 1);
		*ep = fx_cond(cpu, b2 & 15);
		NEXT;
	}
	if (b2 >= 0xc8 && b2 <= 0xcf) {         /* BSWAP */
		REGi(b2 & 7) = __builtin_bswap32(REGi(b2 & 7));
		NEXT;
	}
	u8 *ep;
	u32 *rp = R32P(REG_FIELD(*p));
	switch (b2) {                           /* MOVZX MOVSX IMUL CMOVcc: Gv,E */
	case 0xb1: {                            /* CMPXCHG Ev,Gv */
		FX_E(ep, R32P, 4, 1);
		u32 x = *(u32u *)ep;
		cpu->cc.src1 = x;
		cpu->cc.src2 = REGi(0);
		cpu->cc.dst = x - REGi(0);
		cpu->cc.op = CC_SUB;
		cpu->cc.mask = CF | PF | AF | ZF | SF | OF;
		if (cpu->cc.dst == 0)
			*(u32u *)ep = *rp;
		else
			REGi(0) = x;
		NEXT;
	}
	case 0xc1: {                            /* XADD Ev,Gv */
		FX_E(ep, R32P, 4, 1);
		u32 x = *(u32u *)ep;
		cpu->cc.src1 = x;
		cpu->cc.src2 = *rp;
		cpu->cc.dst = x + *rp;
		cpu->cc.op = CC_ADD;
		cpu->cc.mask = CF | PF | AF | ZF | SF | OF;
		*rp = x;
		*(u32u *)ep = cpu->cc.dst;
		NEXT;
	}
	case 0xb6:
		FX_E(ep, R8P, 1, 0);
		*rp = *ep;
		NEXT;
	case 0xb7:
		FX_E(ep, R32P, 2, 0);
		*rp = *(u16u *)ep;
		NEXT;
	case 0xbe:
		FX_E(ep, R8P, 1, 0);
		*rp = (s8)*ep;
		NEXT;
	case 0xbf:
		FX_E(ep, R32P, 2, 0);
		*rp = (s16)*(u16u *)ep;
		NEXT;
	case 0xaf: {
		FX_E(ep, R32P, 4, 0);
		u32 x = *rp, y = *(u32u *)ep;
		int64_t res = (int64_t)(s32)x * (int64_t)(s32)y;
		cpu->cc.src1 = x;
		cpu->cc.src2 = y;
		cpu->cc.dst = res;
		cpu->cc.dst2 = res >> 32;
		cpu->cc.op = CC_IMUL32;
		cpu->cc.mask = CF | PF | AF | ZF | SF | OF;
		*rp = cpu->cc.dst;
		NEXT;
	}
	}
	if (b2 >= 0x40 && b2 <= 0x4f) {
		FX_E(ep, R32P, 4, 0);
		if (fx_cond(cpu, b2 & 15))
			*rp = *(u32u *)ep;
		NEXT;
	}
	goto fallback;
}

	/* ---- after 0x66 */
w_moffs: {              /* a1 a3: MOV AX,[moffs] / [moffs],AX (Word 95's loops: millions) */
	u8 *m;
	uword ea = *(u32u *)p;
	p += 4;
	FX_MEM(m, obase + ea, 2, b == 0xa3);
	if (b == 0xa1)
		cpu->gprx[0].r16 = *(u16u *)m;
	else
		*(u16u *)m = cpu->gprx[0].r16;
	NEXT;
}
w_shift: {              /* c1 d1 d3: SHL SHR SAR Ew by imm8 / 1 / CL, as i386.c's SHLw.. */
	u8 *ep;
	int op = REG_FIELD(*p);
	if (op != 4 && op != 5 && op != 7)
		goto fallback;
	/* (the count, without consuming it yet: one over 16 stays cpu_exec1's) */
	const u8 *q = p;
	FX_E(ep, R16P, 2, 1);
	uword y = b == 0xc1 ? *p : b == 0xd1 ? 1 : cpu->gprx[1].r8[0];
	y &= 0x1f;
	if (y > 16 && op != 7) {
		p = (u8 *)q;
		goto fallback;
	}
	if (b == 0xc1)
		p++;
	if (y) {
		uword x = *(u16u *)ep;
		if (op == 4) {
			cpu->cc.dst = sext16(x << y);
			cpu->cc.dst2 = (x >> (16 - y)) & 1;
			cpu->cc.op = CC_SHL;
		} else if (op == 5) {
			cpu->cc.src1 = sext16(x);
			cpu->cc.dst = sext16(x >> y);
			cpu->cc.dst2 = (x >> (y - 1)) & 1;
			cpu->cc.op = CC_SHR;
		} else {
			sword sx = sext16(x);
			cpu->cc.dst = sx >> y;
			cpu->cc.dst2 = (sx >> (y - 1)) & 1;
			cpu->cc.op = CC_SAR;
		}
		cpu->cc.mask = CF | PF | ZF | SF | OF;
		*(u16u *)ep = (u16)cpu->cc.dst;
	}
	NEXT;
}
w_alu_evgv: {           /* 01 03 09 0b 21 23 29 2b 31 33 39 3b: Ew,Gw Gw,Ew */
	u8 *ep;
	int op = b >> 3;
	u16u *rp = (u16u *)R16P(REG_FIELD(*p));
	FX_E(ep, R16P, 2, op != 7 && !(b & 2));
	u16u *dp = b & 2 ? rp : (u16u *)ep, *sp = b & 2 ? (u16u *)ep : rp;
	uword r;
	FX_ALU(16, op, *dp, *sp, r);
	if (op != 7)
		*dp = r;
	NEXT;
}
w_alu_axiw: {
	int op = b >> 3;
	uword r;
	FX_ALU(16, op, cpu->gprx[0].r16, *(u16u *)p, r);
	p += 2;
	if (op != 7)
		cpu->gprx[0].r16 = r;
	NEXT;
}
w_grp1: {
	u8 *ep;
	int op = REG_FIELD(*p);
	FX_E(ep, R16P, 2, op != 7);
	u16 y;
	if (b == 0x83)
		y = (s8)*p++;
	else {
		y = *(u16u *)p;
		p += 2;
	}
	uword r;
	FX_ALU(16, op, *(u16u *)ep, y, r);
	if (op != 7)
		*(u16u *)ep = r;
	NEXT;
}
w_test: {
	u8 *ep;
	u16u *rp = (u16u *)R16P(REG_FIELD(*p));
	FX_E(ep, R16P, 2, 0);
	cpu->cc.dst = sext16(*(u16u *)ep & *rp);
	cpu->cc.op = CC_AND;
	cpu->cc.mask = CF | PF | ZF | SF | OF;
	NEXT;
}
w_test_ax:
	cpu->cc.dst = sext16(cpu->gprx[0].r16 & *(u16u *)p);
	p += 2;
	cpu->cc.op = CC_AND;
	cpu->cc.mask = CF | PF | ZF | SF | OF;
	NEXT;
w_mov_89: {
	u8 *ep;
	u16u *rp = (u16u *)R16P(REG_FIELD(*p));
	FX_E(ep, R16P, 2, 1);
	*(u16u *)ep = *rp;
	NEXT;
}
w_mov_8b: {
	u8 *ep;
	u16u *rp = (u16u *)R16P(REG_FIELD(*p));
	FX_E(ep, R16P, 2, 0);
	*rp = *(u16u *)ep;
	NEXT;
}
w_mov_r16i:
	cpu->gprx[b & 7].r16 = *(u16u *)p;
	p += 2;
	NEXT;
w_mov_c7: {
	u8 *ep;
	if (REG_FIELD(*p))
		goto fallback;
	FX_E(ep, R16P, 2, 1);
	*(u16u *)ep = *(u16u *)p;
	p += 2;
	NEXT;
}
w_0f: {                 /* Jcc rel32 (the operand size doesn't change it) */
	u8 b2 = *p++;
	if (b2 < 0x80 || b2 > 0x8f)
		goto fallback;
	s32 d = *(u32u *)p;
	pc = (uintptr_t)p + 4;
	if (fx_cond(cpu, b2 & 15))
		pc += d;
	goto loop;
}

fallback:
	/* the instruction from its start, by cpu_exec1 (which counts it) */
fallback_here:
#ifdef FX_STATS
	{
		extern unsigned fx_stats[0x400];
		const u8 *q = (const u8 *)pc;
		if (fb && pc - wlo < wlen) {
			unsigned k = q[0];
			if (k == 0x64) k = 0x300 | q[1];
			else if (k == 0x66 || k == 0xf3 || k == 0xf2) k = 0x200 | q[1];
			else if (k == 0x0f) k = 0x100 | q[1];
			fx_stats[k]++;
		} else
			fx_stats[0x2ff]++;
	}
#endif
	cpu->next_ip = pc - fb;
	cpu->cycle += steps0 - stepcount - 1;
	cpu->stop = false;
	{
		/* (frank-dos: the overlay's cpu_exec1, EXEC1_CALL; it may count
		 * more than one, MOV SS's shadow taking the next) */
		long c1 = cpu->cycle;
		if (!EXEC1_CALL(cpu, 1))
			return false;
		stepcount -= (int)(cpu->cycle - c1) - 1;
	}
	/* (as cpu_exec1's handlers return: HLT, an interrupt let in) */
	if (cpu->halt || cpu->stop || (cpu->intr && (cpu->flags & IF)))
		return true;
	goto again;
}
#undef NEXT
