/*
 * frank-386 — a JIT for 16-bit real mode (included by i386.c, after cpu_exec1).
 *
 * Blocks of straight-line real-mode code, from a loop head (cpu_exec1's loop
 * enters here when IP moved backwards or more than 15 bytes), compiled once
 * one has been entered JIT_THRESH times. A block is Thumb-2 that calls one
 * helper per guest instruction, its operands decoded at compile time:
 *
 *	mov r0, r4 ; movw/movt r1, desc ; movw/movt r2, aux ; bl helper
 *	cbnz r0, next ; movw r1, #ip ; movs r0, #k ; b exit    (bail)
 *
 * so the fetch from guest memory, the prefix loop, the decode and the
 * dispatch are gone; what an instruction does is the interpreter's own
 * macros (AOP_helper and the rest), so its flags are the interpreter's.
 *
 * A helper returns 0 when it would need the slow path (a TLB miss, a store
 * into a page holding compiled code): nothing has changed, the block returns
 * how many instructions it has done and the interpreter runs that one. A
 * branch, CALL or RET ends a block; its helper sets next_ip.
 *
 * Self-modifying code: a page whose code is compiled is marked (jit_pages),
 * and so is each 128-byte chunk of it a block covers (jit_chunks). The page
 * gets no write tag in the TLB, so a guest store to it takes translate's
 * slow path (translate_laddr), and one into a marked chunk drops the blocks
 * it overlaps (jit_inval). Real-mode code and its data share pages: a whole
 * cache flush per store there emptied it thousands of times a second. The
 * JIT's own stores bail for a marked chunk (jit_wr_ok). DMA, the IDE and
 * INT 13h disks and the H: redirector write guest memory without the CPU:
 * they call jit_note_write().
 *
 * Only real mode (CR0.PE 0): blocks are flushed when protected mode is seen.
 */

/* SRAM code and data: the overlay's JIT variant (i386.c, EXEC1_OVERLAY) */
#if EXEC1_OVERLAY
#define JIT_RAM(name) __attribute__((section(".ovl_jit." #name), noinline)) name
#define JIT_DATA(name) __attribute__((section(".ovl_jit." #name)))
#else
#define JIT_RAM(name) __not_in_flash_func(name)
#define JIT_DATA(name)
#endif

#ifndef JIT_CACHE_BYTES
#define JIT_CACHE_BYTES (28 * 1024)
#endif
#define JIT_TAB_N       2048u
#ifndef JIT_STATS                       /* (per block run: off on the board unless profiling) */
#if defined(PICO_ON_DEVICE) && !JIT_PROF
#define JIT_STATS 0
#else
#define JIT_STATS 1
#endif
#endif
#ifndef JIT_THRESH
#define JIT_THRESH      32u
#endif
#ifndef JIT_MAX_INSNS
#define JIT_MAX_INSNS   32u
#endif
#define JIT_NO_CODE     0xFFFFu
#define JIT_LIN_END     0x110000u        /* real mode's reach: 1 MB + the HMA */
#define JIT_PAGES       (JIT_LIN_END >> 12)
#define JIT_CHUNK_SH    7
#define JIT_DROPPED     0x80u
#define JIT_FALLS       0x40u   /* (count, compiled) it ends before an instruction it doesn't take */
#define JIT_NINSNS      0x3fu   /* (count, compiled) its instructions at most */
#define JIT_EVICTED     0x40u   /* (count, not compiled) its code was evicted: compiled at its next entry */
_Static_assert(JIT_MAX_INSNS <= JIT_NINSNS, "count's low bits");
#define JIT_MAX_BYTES   240u    /* (len is a byte) */

struct jit_ent {
	uint32_t key;           /* the block's CS base >> 4 << 16 | IP (its code has IPs in it) */
	uint8_t count;          /* entries until compiled, then its instructions (JIT_NINSNS);
				 * JIT_DROPPED: a store hit its code; JIT_FALLS */
	uint8_t len;            /* the guest bytes it covers */
	uint16_t off;           /* halfword offset + 1 into jit_code; 0 none; JIT_NO_CODE: can't */
};

static uint16_t jit_code[JIT_CACHE_BYTES / 2] __attribute__((aligned(4))) JIT_DATA(jit_code);
static uint32_t jit_used;               /* halfwords: where the next block goes */
static uint32_t jit_free_end = JIT_CACHE_BYTES / 2;    /* [jit_used, jit_free_end): no live block's code */
static struct jit_ent jit_tab[JIT_TAB_N] JIT_DATA(jit_tab);
uint32_t jit_pages[(JIT_PAGES + 31) / 32];
static uint32_t jit_chunks[(JIT_LIN_END >> JIT_CHUNK_SH) / 32];
#if JIT_PROF
uint32_t jit_prof_pfx[JIT_PROF_PFX ? 3 : 1][256];          /* after 66, 67, 26: the opcode (0F xx as xx) */
uint32_t jit_prof_lin[256], jit_prof_lpos; bool jit_prof_instep;
uint32_t jit_prof_op[256];              /* the interpreter's real-mode instructions by first byte (over SWD) */
#endif
uint32_t jit_stat_runs, jit_stat_insns, jit_stat_blocks, jit_stat_flushes, jit_stat_bails, jit_stat_invals, jit_stat_evicts,
	jit_stat_rekeys, jit_stat_steps2, jit_stat_recomp;

/* frank-dos: the threshold under thrashing. When the code cache churns
 * (evictions: a working set bigger than the cache, as Doom 286's), an entry
 * is counted towards JIT_THRESH only once in (jit_scale_mask + 1), on
 * average: only the really hot blocks are compiled, and they stay. Every
 * JIT_ADAPT_EVERY blocks compiled, the mask doubles if there were evictions
 * meanwhile, and halves if there were none. (The count itself has 6 bits.) */
#define JIT_ADAPT_EVERY 256u
static uint32_t jit_scale_mask JIT_DATA(jit_scale_mask);
static uint32_t jit_rnd JIT_DATA(jit_rnd) = 0x12345678u;
static uint32_t jit_adapt_blocks JIT_DATA(jit_adapt_blocks), jit_adapt_evicts JIT_DATA(jit_adapt_evicts);
uint32_t jit_stat_scale;                    /* (the console's ~C jit~: the mask now) */

static inline bool jit_count_this(void)
{
	if (!jit_scale_mask)
		return true;
	jit_rnd = jit_rnd * 1664525u + 1013904223u;
	return ((jit_rnd >> 24) & jit_scale_mask) == 0;
}

static void jit_adapt(void)
{
	if (++jit_adapt_blocks < JIT_ADAPT_EVERY)
		return;
	jit_adapt_blocks = 0;
	uint32_t ev = jit_stat_evicts - jit_adapt_evicts;
	jit_adapt_evicts = jit_stat_evicts;
	if (ev > 4) {
		if (jit_scale_mask < 63)
			jit_scale_mask = jit_scale_mask * 2 + 1;
	} else if (ev == 0)
		jit_scale_mask >>= 1;
	jit_stat_scale = jit_scale_mask;
}

static inline bool jit_page_marked(uword page)
{
	return page < JIT_PAGES && (jit_pages[page >> 5] >> (page & 31) & 1);
}

/* stores already found to miss every block (lin << 3 | len): data next to
 * code in a chunk, written again and again. Emptied when a block is made. */
#define JIT_SAFE_N 16u
static uint32_t jit_safe[JIT_SAFE_N];

static uint32_t jit_drops;              /* blocks jit_inval dropped */

static inline bool jit_chunk_hit(uword lin, uword len)
{
	if (lin >= JIT_LIN_END)
		return false;
	uword last = lin + len - 1 < JIT_LIN_END ? lin + len - 1 : JIT_LIN_END - 1;
	for (uword c = lin >> JIT_CHUNK_SH; c <= last >> JIT_CHUNK_SH; c++)
		if (jit_chunks[c >> 5] >> (c & 31) & 1)
			return true;
	return false;
}

static void jit_flush(void)
{
	/* a block that had code is compiled again at its next entry: a full
	 * cache would otherwise send every hot block back to the interpreter
	 * for JIT_THRESH entries */
	for (unsigned i = 0; i < JIT_TAB_N; i++) {
		struct jit_ent *e = &jit_tab[i];
		e->count = e->off && e->off != JIT_NO_CODE ? JIT_EVICTED | (JIT_THRESH - 1) : 0;
		e->off = 0;
	}
	memset(jit_pages, 0, sizeof jit_pages);
	memset(jit_chunks, 0, sizeof jit_chunks);
	memset(jit_safe, 0, sizeof jit_safe);
	jit_used = 0;
	jit_free_end = JIT_CACHE_BYTES / 2;
	jit_stat_flushes++;
}

/* Room for need halfwords at jit_used: the cache is a ring, and the oldest
 * blocks after jit_used are dropped JIT_EVICT halfwords at a time (to be
 * compiled again at their next entry). A full flush made every hot block
 * start over whenever Wolf3D's working set overflowed the cache. */
#define JIT_EVICT 1024u
static void jit_make_room(uint32_t need)
{
	if (jit_used + need > JIT_CACHE_BYTES / 2) {
		jit_used = 0;
		jit_free_end = 0;
	}
	while (jit_free_end < jit_used + need) {
		uint32_t lo = jit_free_end, hi = lo + JIT_EVICT;
		for (unsigned i = 0; i < JIT_TAB_N; i++) {
			struct jit_ent *e = &jit_tab[i];
			if (e->off && e->off != JIT_NO_CODE && e->off - 1u >= lo && e->off - 1u < hi) {
				e->off = 0;
				/* (JIT_EVICTED: the stats can tell its compile from a new block's) */
				e->count = (e->count & JIT_DROPPED) | JIT_EVICTED | (JIT_THRESH - 1);
			}
		}
		jit_free_end = hi;
		jit_stat_evicts++;
	}
}

/* lin..lin+len-1 is written: the blocks it overlaps are dropped (their code
 * stays in the cache, unreachable, until it is flushed), and its chunks are
 * unmarked when no block left has code in them. */
static void jit_inval(uword lin, uword len)
{
	if (!len || !jit_chunk_hit(lin, len))
		return;
	uint32_t *safe = &jit_safe[(lin >> 1) & (JIT_SAFE_N - 1)];
	if (len <= 4 && *safe >> 3 == lin && (*safe & 7) >= len)
		return;
	bool dropped = false;
	uword c0 = lin >> JIT_CHUNK_SH, c1 = (lin + len - 1) >> JIT_CHUNK_SH;
	uword lo = c0 << JIT_CHUNK_SH, hi = (c1 + 1) << JIT_CHUNK_SH;
	bool keep = false;
	for (unsigned i = 0; i < JIT_TAB_N; i++) {
		struct jit_ent *e = &jit_tab[i];
		if (!e->off || e->off == JIT_NO_CODE)
			continue;
		uword b = (e->key >> 16 << 4) + (e->key & 0xffff);
		if (!(lo < b + e->len && b < hi))
			continue;
		if (!(lin < b + e->len && b < lin + len)) {
			keep = true;
			continue;
		}
		{
			/* the second time: code rewritten as it runs, left to the interpreter */
			e->off = e->count & JIT_DROPPED ? JIT_NO_CODE : 0;
			e->count = JIT_DROPPED;
			jit_drops++;
			dropped = true;
		}
	}
	if (!dropped && len <= 4)
		*safe = lin << 3 | len;
	if (!keep && c1 < (JIT_LIN_END >> JIT_CHUNK_SH))
		for (uword c = c0; c <= c1; c++)
			jit_chunks[c >> 5] &= ~(1u << (c & 31));
	jit_stat_invals++;
}

/* (guest_mem.h, disk.c, netredirect.c: memory written without the CPU) */
void jit_note_write(uint32_t addr, uint32_t len)
{
	jit_inval(addr, len);
}

/* ---- operands ---- */

/* desc: [2:0] rm, [5:3] reg, [7:6] mod, [10:8] segment (7: the default),
 * bit 11 32-bit addressing (67h), [13:12] the operand width (0 byte, 1 word,
 * 2 dword: 66h), bit 14 the displacement is the helper's x;
 * [31:16] the displacement (16-bit addressing: disp8 sign-extended, or
 * disp16), or [23:16] the SIB byte (32-bit addressing).
 * x (r3): the immediate, or the 32-bit displacement (D_XDISP). */
#define D_RM(d)  ((d) & 7)
#define D_REG(d) (((d) >> 3) & 7)
#define D_MOD(d) (((d) >> 6) & 3)
#define D_SEG(d) (((d) >> 8) & 7)
#define D_A32    (1u << 11)
#define D_W(d)   (((d) >> 12) & 3)
#define D_XDISP  (1u << 14)
#define D_DISP(d) ((d) >> 16)
#define D_SIB(d) (((d) >> 16) & 0xff)

static inline __attribute__((always_inline)) uword jit_ea(CPUI386 *cpu, uint32_t d, uint32_t x, int *seg)
{
	int rm = D_RM(d), mod = D_MOD(d), s = D_SEG(d);
	uword a;
	bool ss;
	if (!(d & D_A32)) {
		if (rm == 6 && mod == 0)
			a = D_DISP(d);
		else {
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
			a += D_DISP(d);
		}
		a &= 0xffff;
		ss = rm == 2 || rm == 3 || (mod != 0 && rm == 6);
	} else {
		uword disp = d & D_XDISP ? x : 0;
		if (rm == 4) {
			int sib = D_SIB(d), base = sib & 7, idx = (sib >> 3) & 7;
			a = idx == 4 ? 0 : REGi(idx) << (sib >> 6);
			if (base == 5 && mod == 0)
				ss = false;
			else {
				a += REGi(base);
				ss = base == 4 || base == 5;
			}
		} else if (rm == 5 && mod == 0) {
			a = 0;
			ss = false;
		} else {
			a = REGi(rm);
			ss = rm == 5;
		}
		a += disp;
	}
	*seg = s != 7 ? s : ss ? SEG_SS : SEG_DS;
	return a;
}

/* may this store go ahead (not into compiled code)? */
static inline __attribute__((always_inline)) bool jit_wr_ok(CPUI386 *cpu, int seg, uword addr, int size)
{
	return !jit_chunk_hit(cpu->seg[seg].base + addr, size);
}

/* the TLB missed: the full translation, but never a store into compiled code.
 * A store to a page that has code, outside its code's chunks, goes through
 * the read tag: an identity entry (tlb_tag_identity) has a write tag exactly
 * when it has a read tag but for the JIT, so it is the store a build without
 * the JIT makes. (Tiny-model code keeps its data in the code's pages.) */
static bool JIT_RAM(jit_mem_slow)(CPUI386 *cpu, OptAddr *res, int rwm, int seg, uword addr, int size)
{
	if ((rwm & 2) && !jit_wr_ok(cpu, seg, addr, size))
		return false;
	if (rwm & 2) {
		uword lin = cpu->seg[seg].base + addr, lim = cpu->seg[seg].limit, pg = lin >> 12;
		struct tlb_entry *e = &cpu->tlb.tab[pg % tlb_size];
		if (e->rtag == pg && e->lpgno == pg && e->xaddr == 0 && e->hbase == (uintptr_t)cpu->phys_mem &&
		    jit_page_marked(pg) && (lin & 0xfff) <= 0x1000u - size && addr <= lim && lim - addr >= (uword)size - 1) {
			res->res = ADDR_OK1;
			res->addr1 = lin;
			return true;
		}
	}
	return translate(cpu, res, rwm, seg, addr, size, cpu->cpl);
}

/* seg:addr for SIZE bytes into m; false = bail. The TLB's hit inline: a page
 * holding compiled code never has a write tag, so a write hit needs no other
 * check (jit_compile clears it, tlb_tag_identity doesn't set it). Past the
 * segment's limit (a word at FFFFh, a 67h offset above it): the slow path. */
#define JIT_AT(m, RWM, sg, addr, SIZE) ({ \
	uword _lin = cpu->seg[sg].base + (addr), _lim = cpu->seg[sg].limit; \
	struct tlb_entry *_e = &cpu->tlb.tab[(_lin >> 12) % tlb_size]; \
	bool _ok = true; \
	if (likely((((RWM) & 2) ? _e->wtag : _e->rtag) == (_lin >> 12) && \
		   (_lin & 0xfff) <= 0x1000u - (SIZE) && (addr) <= _lim && _lim - (addr) >= (uword)(SIZE) - 1)) { \
		(m)->res = ADDR_OK1; \
		(m)->addr1 = _e->xaddr ^ _lin; \
	} else \
		_ok = jit_mem_slow(cpu, (m), RWM, sg, addr, SIZE); \
	_ok; })

/* the E operand's memory (desc, x): meml, its host address, for width w
 * (RWM & 2: to be stored); bails */
#define JIT_EM(RWM, w) \
	uintptr_t meml; \
	{ \
		int seg; \
		uword addr = jit_ea(cpu, d, x, &seg); \
		meml = JIT_HP(seg, addr, 1 << (w), ((RWM) & 2) != 0); \
		if (!meml) \
			return 0; \
	}

/* by width (0 byte, 1 word, 2 dword) */
static inline __attribute__((always_inline)) uint32_t jit_lr(CPUI386 *cpu, int r, int w)
{
	return w == 0 ? lreg8(r) : w == 1 ? lreg16(r) : lreg32(r);
}
static inline __attribute__((always_inline)) void jit_sr(CPUI386 *cpu, int r, int w, uint32_t v)
{
	if (w == 0) sreg8(r, v); else if (w == 1) sreg16(r, v); else sreg32(r, v);
}
static inline __attribute__((always_inline)) uint32_t jit_lm(CPUI386 *cpu, OptAddr *m, int w)
{
	return w == 0 ? laddr8(m) : w == 1 ? laddr16(m) : laddr32(m);
}
static inline __attribute__((always_inline)) void jit_sm(CPUI386 *cpu, OptAddr *m, int w, uint32_t v)
{
	if (w == 0) saddr8(m, v); else if (w == 1) saddr16(m, v); else saddr32(m, v);
}
static inline __attribute__((always_inline)) int32_t jit_sx(uint32_t v, int w)
{
	return w == 0 ? (s8)v : w == 1 ? (s16)v : (s32)v;
}

/* E as a local: v loaded from the register or memory (rwm 3 when st),
 * operated on by cpu_exec1's macros through lvb/svb, lvw/svw, lvd/svd,
 * stored back by JIT_E_PUT */
#define JIT_E_GET(w, st) \
	int rm = D_RM(d); \
	bool mem = D_MOD(d) != 3; \
	uintptr_t meml = 0; \
	uint32_t v; \
	if (mem) { \
		int seg; \
		uword addr = jit_ea(cpu, d, x, &seg); \
		meml = JIT_HP(seg, addr, 1 << (w), (st) != 0); \
		if (!meml) \
			return 0; \
		v = jit_rdh(meml, w); \
	} else \
		v = jit_lr(cpu, rm, w);
#define JIT_E_PUT(w) do { \
	if (mem) jit_wrh(meml, w, v); \
	else jit_sr(cpu, rm, w, v); \
} while (0)
#define lvb(a) ((u8)v)
#define svb(a, x) (v = (u8)(x))
#define lvw(a) ((u16)v)
#define svw(a, x) (v = (u16)(x))
#define lvd(a) ((u32)v)
#define svd(a, x) (v = (u32)(x))

#define JIT_SEGOFF(sg, f) (offsetof(CPUI386, seg) + (sg) * sizeof(((CPUI386 *)0)->seg[0]) + \
			   offsetof(__typeof__(((CPUI386 *)0)->seg[0]), f))
/* desc (r2) of a memory operand: [7:0] the offset of its segment's base in
 * CPUI386, [10:8] size - 1, [14:12] the segment, bit 15 a store */
#define JIT_MDESC(sg, size, wr) (JIT_SEGOFF(sg, base) | ((size) - 1) << 8 | (sg) << 12 | (wr) << 15)

static uintptr_t jit_mem_c(CPUI386 *cpu, uword off, uint32_t desc);

/* the host address of size bytes at sg:off (wr: to be stored), 0 when the
 * slow path can't (the helper then bails); and access through it */
#define JIT_HP(sg, off, size, wr) jit_mem_c(cpu, (off), JIT_MDESC(sg, size, wr))
static inline __attribute__((always_inline)) uint32_t jit_rdh(uintptr_t h, int w)
{
	return w == 0 ? *(u8 *)h : w == 1 ? *(u16 *)h : *(u32 *)h;
}
static inline __attribute__((always_inline)) void jit_wrh(uintptr_t h, int w, uint32_t v)
{
	if (w == 0) *(u8 *)h = v; else if (w == 1) *(u16 *)h = v; else *(u32 *)h = v;
}

#define JH static int JIT_RAM
/* the rarer helpers, in flash (SRAM is short): called through a register */
#define JHF static int __attribute__((noinline))

/* x OP y, the 8 ALU operations of 00..3F and group 1 by index (8: TEST),
 * width w; the lazy flags as cpu_exec1's AOP/LOP/ACOP helpers set them.
 * The result (sign-extended). */
static inline __attribute__((always_inline)) uint32_t jit_alu_core(CPUI386 *cpu, int op, uint32_t x, uint32_t y, int w)
{
	int32_t a = jit_sx(x, w), c = jit_sx(y, w), r;
	switch (op) {
	case 1: r = a | c; cpu->cc.op = CC_OR; goto logic;
	case 4: case 8: r = a & c; cpu->cc.op = CC_AND; goto logic;
	case 6: r = a ^ c; cpu->cc.op = CC_XOR; goto logic;
	case 0: r = a + c; cpu->cc.op = CC_ADD; break;
	case 2: { int cf = get_CF(cpu); r = a + c + cf; cpu->cc.op = cf ? CC_ADC : CC_ADD; break; }
	case 3: { int cf = get_CF(cpu); r = a - c - cf; cpu->cc.op = cf ? CC_SBB : CC_SUB; break; }
	default: r = a - c; cpu->cc.op = CC_SUB; break;
	}
	r = jit_sx(r, w);
	cpu->cc.src1 = a;
	cpu->cc.src2 = c;
	cpu->cc.dst = r;
	cpu->cc.mask = CF | PF | AF | ZF | SF | OF;
	return r;
logic:
	cpu->cc.dst = r;
	cpu->cc.mask = CF | PF | ZF | SF | OF;
	return r;
}

/* ALU and TEST, every form: aux [3:0] the operation (8 TEST), bit 5 into G
 * (Gb,Eb / Gv,Ev), bit 6 the source is the immediate x (the accumulator
 * forms: desc mod 3, rm 0) */
JH(jh_alu)(CPUI386 *cpu, uint32_t d, uint32_t aux, uint32_t x)
{
	int op = aux & 15, rm = D_RM(d), reg = D_REG(d), w = D_W(d);
	bool tog = aux & 32;
	uint32_t y = (aux & 64) ? x : jit_lr(cpu, reg, w);
	if (D_MOD(d) == 3) {
		uint32_t a = jit_lr(cpu, rm, w);
		if (tog) { uint32_t t = a; a = y; y = t; }
		uint32_t r = jit_alu_core(cpu, op, a, y, w);
		if (op < 7)
			jit_sr(cpu, tog ? reg : rm, w, r);
		return 1;
	}
	bool st = !tog && op < 7;
	JIT_EM(st ? 3 : 1, w)
	uint32_t m = jit_rdh(meml, w);
	if (tog) {
		uint32_t r = jit_alu_core(cpu, op, y, m, w);
		if (op < 7)
			jit_sr(cpu, reg, w, r);
		return 1;
	}
	uint32_t r = jit_alu_core(cpu, op, m, y, w);
	if (st)
		jit_wrh(meml, w, r);
	return 1;
}

/* MOV: aux 0 E,G  1 G,E  2 E,imm (x) */
JHF jh_mov(CPUI386 *cpu, uint32_t d, uint32_t aux, uint32_t x)
{
	int rm = D_RM(d), reg = D_REG(d), w = D_W(d);
	if (D_MOD(d) == 3) {
		if (aux == 1) jit_sr(cpu, reg, w, jit_lr(cpu, rm, w));
		else jit_sr(cpu, rm, w, aux == 2 ? x : jit_lr(cpu, reg, w));
		return 1;
	}
	if (aux == 1) {
		JIT_EM(1, w)
		jit_sr(cpu, reg, w, jit_rdh(meml, w));
		return 1;
	}
	uint32_t v = aux == 2 ? x : jit_lr(cpu, reg, w);
	JIT_EM(2, w)
	jit_wrh(meml, w, v);
	return 1;
}

/* MOVZX / MOVSX G,E: aux bit 0 the source is a word, bit 1 sign-extended */
JH(jh_movx)(CPUI386 *cpu, uint32_t d, uint32_t aux, uint32_t x)
{
	int sw = aux & 1;
	uint32_t v;
	if (D_MOD(d) == 3)
		v = jit_lr(cpu, D_RM(d), sw);
	else {
		JIT_EM(1, sw)
		v = jit_rdh(meml, sw);
	}
	if (aux & 2)
		v = jit_sx(v, sw);
	jit_sr(cpu, D_REG(d), D_W(d), v);
	return 1;
}

/* INC/DEC: 40..4F (aux = reg | 32), FE/FF /0 /1 (desc); aux bit 3: DEC */
JH(jh_incdec)(CPUI386 *cpu, uint32_t d, uint32_t aux, uint32_t x)
{
	bool dec = aux & 8;
	int w = D_W(d);
	if (aux & 32)
		d = (d & ~0xffu) | 0xc0 | (aux & 7);
	JIT_E_GET(w, 1)
	if (w == 0) { if (dec) { DECb(0, lvb, svb) } else { INCb(0, lvb, svb) } }
	else if (w == 1) { if (dec) { DECw(0, lvw, svw) } else { INCw(0, lvw, svw) } }
	else { if (dec) { DECd(0, lvd, svd) } else { INCd(0, lvd, svd) } }
	JIT_E_PUT(w);
	return 1;
}

/* the stack (real mode: SS's B bit is clear, SP wraps at 64K); w 1 or 2 */
static int JIT_RAM(jit_push)(CPUI386 *cpu, uint32_t v, int w)
{
	uword sp = (lreg16(4) - (1u << w)) & 0xffff;
	uintptr_t h = JIT_HP(SEG_SS, sp, 1 << w, 1);
	if (!h)
		return 0;
	jit_wrh(h, w, v);
	sreg16(4, sp);
	return 1;
}

/* PUSH r (aux = reg), PUSH imm (aux 0x100, x) */
JH(jh_push)(CPUI386 *cpu, uint32_t d, uint32_t aux, uint32_t x)
{
	int w = D_W(d);
	return jit_push(cpu, aux & 0x100 ? x : jit_lr(cpu, aux & 7, w), w);
}

JH(jh_pop)(CPUI386 *cpu, uint32_t d, uint32_t aux, uint32_t x)
{
	(void)x;
	int w = D_W(d);
	uword sp = lreg16(4);
	uintptr_t h = JIT_HP(SEG_SS, sp, 1 << w, 0);
	if (!h)
		return 0;
	uint32_t v = jit_rdh(h, w);
	sreg16(4, (sp + (1u << w)) & 0xffff);
	jit_sr(cpu, aux & 7, w, v);     /* (POP SP: the popped value, as cpu_exec1) */
	return 1;
}

JH(jh_lea)(CPUI386 *cpu, uint32_t d, uint32_t aux, uint32_t x)
{
	(void)aux;
	int seg;
	jit_sr(cpu, D_REG(d), D_W(d), jit_ea(cpu, d, x, &seg));
	return 1;
}

/* XCHG E,G; XCHG eAX,r (aux = reg | 0x100) */
JHF jh_xchg(CPUI386 *cpu, uint32_t d, uint32_t aux, uint32_t x)
{
	int w = D_W(d);
	if (aux & 0x100) {
		int r = aux & 7;
		uint32_t t = jit_lr(cpu, 0, w);
		jit_sr(cpu, 0, w, jit_lr(cpu, r, w));
		jit_sr(cpu, r, w, t);
		return 1;
	}
	int reg = D_REG(d);
	JIT_E_GET(w, 1)
	uint32_t g = jit_lr(cpu, reg, w);
	jit_sr(cpu, reg, w, v);
	v = g;
	JIT_E_PUT(w);
	return 1;
}

/* shifts, group 2: desc (reg = the operation), aux = count source:
 * 0 one, 1 CL, 2 imm (in aux >> 8) */
JH(jh_shift)(CPUI386 *cpu, uint32_t d, uint32_t aux, uint32_t x)
{
	int op = D_REG(d), w = D_W(d);
	uword n = (aux & 3) == 0 ? 1 : (aux & 3) == 1 ? lreg8(1) : (aux >> 8) & 0xff;
	JIT_E_GET(w, 1)
#define JIT_SH(BW, la, sa) \
	switch (op) { \
	case 0: { ROL ## BW(0, n, la, sa, limm, 0) break; } \
	case 1: { ROR ## BW(0, n, la, sa, limm, 0) break; } \
	case 2: { RCL ## BW(0, n, la, sa, limm, 0) break; } \
	case 3: { RCR ## BW(0, n, la, sa, limm, 0) break; } \
	case 5: { SHR ## BW(0, n, la, sa, limm, 0) break; } \
	case 7: { SAR ## BW(0, n, la, sa, limm, 0) break; } \
	default: { SHL ## BW(0, n, la, sa, limm, 0) break; } \
	}
	if (w == 0) { JIT_SH(b, lvb, svb) } else if (w == 1) { JIT_SH(w, lvw, svw) } else { JIT_SH(d, lvd, svd) }
#undef JIT_SH
	JIT_E_PUT(w);
	return 1;
}

/* group 3 /2 NOT, /3 NEG, /4 MUL, /5 IMUL (DIV can fault: not compiled) */
JHF jh_grp3(CPUI386 *cpu, uint32_t d, uint32_t aux, uint32_t x)
{
	(void)aux;
	int op = D_REG(d), w = D_W(d);
	JIT_E_GET(w, op <= 3)
#define JIT_G3(BW, la, sa) \
	switch (op) { \
	case 2: { NOT ## BW(0, la, sa) break; } \
	case 3: { NEG ## BW(0, la, sa) break; } \
	case 4: { MUL ## BW(0, la, sa) break; } \
	default: { IMUL ## BW(0, la, sa) break; } \
	}
	if (w == 0) { JIT_G3(b, lvb, svb) } else if (w == 1) { JIT_G3(w, lvw, svw) } else { JIT_G3(d, lvd, svd) }
#undef JIT_G3
	if (op <= 3)
		JIT_E_PUT(w);
	return 1;
}

/* segment registers in real mode (set_seg, as cpu_exec1; not CS, not SS:
 * its interrupt shadow is cpu_exec1's). aux [2:0]: 0 MOV Sw,Ew (seg D_REG);
 * 1 MOV Ew,Sw; 2 PUSH (seg aux >> 8, width D_W); 3 POP; 4 LxS Gv,Mp */
JHF jh_sreg(CPUI386 *cpu, uint32_t d, uint32_t aux, uint32_t x)
{
	int sg = aux >> 8, w = D_W(d);
	switch (aux & 7) {
	case 0: {
		uint32_t v;
		if (D_MOD(d) == 3)
			v = lreg16(D_RM(d));
		else {
			JIT_EM(1, 1)
			v = *(u16 *)meml;
		}
		return set_seg(cpu, D_REG(d), v);
	}
	case 1:
		if (D_MOD(d) == 3)
			sreg16(D_RM(d), cpu->seg[D_REG(d)].sel);
		else {
			JIT_EM(2, 1)
			*(u16 *)meml = cpu->seg[D_REG(d)].sel;
		}
		return 1;
	case 2:
		return jit_push(cpu, cpu->seg[sg].sel, w);
	case 3: {
		uword sp = lreg16(4);
		uintptr_t h = JIT_HP(SEG_SS, sp, 1 << w, 0);
		if (!h)
			return 0;
		uint32_t v = jit_rdh(h, w);
		if (!set_seg(cpu, sg, v))
			return 0;
		sreg16(4, (sp + (1u << w)) & 0xffff);
		return 1;
	}
	default: {                              /* LES LDS LFS LGS */
		int seg;
		uword addr = jit_ea(cpu, d, x, &seg);
		uintptr_t h = JIT_HP(seg, addr, (1 << w) + 2, 0);
		if (!h)
			return 0;
		uint32_t off = jit_rdh(h, w);
		if (!set_seg(cpu, sg, *(u16 *)(h + (1 << w))))
			return 0;
		jit_sr(cpu, D_REG(d), w, off);
		return 1;
	}
	}
}

/* CALL ptr16:16|32: aux = the selector | the IP after it << 16, x = the
 * offset, width D_W; the stack's slots checked first, as CALLFAR */
JH(jh_callf)(CPUI386 *cpu, uint32_t d, uint32_t aux, uint32_t x)
{
	int w = D_W(d), sz = 1 << w;
	uword sp = lreg16(4);
	uintptr_t h1 = JIT_HP(SEG_SS, (sp - sz) & 0xffff, sz, 1), h2 = JIT_HP(SEG_SS, (sp - 2 * sz) & 0xffff, sz, 1);
	if (!h1 || !h2)
		return 0;
	sreg16(4, (sp - 2 * sz) & 0xffff);
	jit_wrh(h1, w, cpu->seg[SEG_CS].sel);
	jit_wrh(h2, w, aux >> 16);
	if (!set_seg(cpu, SEG_CS, aux & 0xffff))
		return 0;
	cpu->next_ip = x & 0xffff;
	return 1;
}

/* RETF [imm16] (aux = imm16), width D_W, as RETFARw */
JH(jh_retf)(CPUI386 *cpu, uint32_t d, uint32_t aux, uint32_t x)
{
	(void)x;
	int w = D_W(d), sz = 1 << w;
	uword sp = lreg16(4);
	uintptr_t h1 = JIT_HP(SEG_SS, sp, sz, 0), h2 = JIT_HP(SEG_SS, (sp + sz) & 0xffff, sz, 0);
	if (!h1 || !h2)
		return 0;
	uword ip = jit_rdh(h1, w);
	if (!set_seg(cpu, SEG_CS, jit_rdh(h2, w)))
		return 0;
	sreg16(4, (sp + 2 * sz + aux) & 0xffff);
	cpu->next_ip = ip & 0xffff;
	return 1;
}

/* FF /2 CALL Ev, /3 CALL Mp, /4 JMP Ev, /5 JMP Mp, /6 PUSH Ev (aux = the
 * IP after it), width D_W; the target read first, as cpu_exec1 */
JH(jh_ff)(CPUI386 *cpu, uint32_t d, uint32_t aux, uint32_t x)
{
	int op = D_REG(d), w = D_W(d), sz = 1 << w;
	bool far = op == 3 || op == 5;
	uint32_t v, sel = 0;
	if (D_MOD(d) == 3) {
		if (far)
			return 0;
		v = jit_lr(cpu, D_RM(d), w);
	} else {
		int seg;
		uword addr = jit_ea(cpu, d, x, &seg);
		uintptr_t h = JIT_HP(seg, addr, far ? sz + 2 : sz, 0);
		if (!h)
			return 0;
		v = jit_rdh(h, w);
		if (far)
			sel = *(u16 *)(h + sz);
	}
	if (op == 6)
		return jit_push(cpu, v, w);
	if (op == 3) {                          /* CALL far: CS, IP */
		uword sp = lreg16(4);
		uintptr_t h1 = JIT_HP(SEG_SS, (sp - sz) & 0xffff, sz, 1), h2 = JIT_HP(SEG_SS, (sp - 2 * sz) & 0xffff, sz, 1);
		if (!h1 || !h2)
			return 0;
		sreg16(4, (sp - 2 * sz) & 0xffff);
		jit_wrh(h1, w, cpu->seg[SEG_CS].sel);
		jit_wrh(h2, w, aux);
	} else if (op == 2 && !jit_push(cpu, aux, w))
		return 0;
	if (far && !set_seg(cpu, SEG_CS, sel))
		return 0;
	cpu->next_ip = v & 0xffff;
	return 1;
}

/* POP Ev (8F /0), as POP_helper: the value from SS:SP, SP on, then the
 * operand's address (with the new SP), SP back if that store can't */
JH(jh_popm)(CPUI386 *cpu, uint32_t d, uint32_t aux, uint32_t x)
{
	(void)aux;
	int w = D_W(d), sz = 1 << w;
	uword sp = lreg16(4);
	uintptr_t h = JIT_HP(SEG_SS, sp, sz, 0);
	if (!h)
		return 0;
	uint32_t v = jit_rdh(h, w);
	sreg16(4, (sp + sz) & 0xffff);
	if (D_MOD(d) == 3) {
		jit_sr(cpu, D_RM(d), w, v);
		return 1;
	}
	int seg;
	uword addr = jit_ea(cpu, d, x, &seg);
	uintptr_t hm = JIT_HP(seg, addr, sz, 1);
	if (!hm) {
		sreg16(4, sp);
		return 0;
	}
	jit_wrh(hm, w, v);
	return 1;
}

/* INT n (desc = n, aux = the IP after it), as INT(): call_isr, the HLE
 * services behind it included */
JH(jh_int)(CPUI386 *cpu, uint32_t d, uint32_t aux, uint32_t x)
{
	(void)x;
	uword oldip = cpu->ip;
	cpu->ip = aux;
	cpu->next_ip = aux;
	if (!call_isr(cpu, d & 0xff, false, 0)) {
		cpu->ip = oldip;
		return 0;
	}
	return 1;
}

/* IRET, 16-bit, real mode: as IRET()'s */
JH(jh_iret)(CPUI386 *cpu, uint32_t d, uint32_t aux, uint32_t x)
{
	(void)d; (void)aux; (void)x;
	uword sp = lreg16(4);
	uintptr_t h1 = JIT_HP(SEG_SS, sp, 2, 0), h2 = JIT_HP(SEG_SS, (sp + 2) & 0xffff, 2, 0),
		  h3 = JIT_HP(SEG_SS, (sp + 4) & 0xffff, 2, 0);
	if (!h1 || !h2 || !h3)
		return 0;
	uword newip = *(u16 *)h1, newcs = *(u16 *)h2, oldflags = cpu->flags;
	cpu->flags = ((cpu->flags & 0xffff0000) | *(u16 *)h3) & EFLAGS_MASK;
	cpu->flags |= 0x2;
	if (!set_seg(cpu, SEG_CS, newcs)) {
		cpu->flags = oldflags;
		return 0;
	}
	cpu->cc.mask = 0;
	sreg16(4, (sp + 6) & 0xffff);
	cpu->next_ip = newip;
	return 1;
}

/* x87, D8..DF (aux = the opcode - D8): fpu.c's, as cpu_exec1's ESC(); a
 * fault (#NM, the operand's) bails, and the interpreter raises it */
JH(jh_esc)(CPUI386 *cpu, uint32_t d, uint32_t aux, uint32_t x)
{
	if ((cpu->cr0 & 0xc) || !cpu->fpu)
		return 0;
	int group = D_REG(d);
	if (D_MOD(d) == 3)
		return fpu_exec1(cpu->fpu, cpu, aux, group, D_RM(d));
	int seg;
	uword addr = jit_ea(cpu, d, x, &seg);
	return fpu_exec2(cpu->fpu, cpu, D_W(d) != 2, aux, group, seg, addr);
}

/* the one-byte flag and conversion instructions: aux = the opcode */
JH(jh_misc)(CPUI386 *cpu, uint32_t d, uint32_t aux, uint32_t x)
{
	(void)x;
	bool o32 = D_W(d) == 2;
	switch (aux) {
	case 0x98:                                                       /* CBW, CWDE */
		if (o32) sreg32(0, sext16(lreg16(0))); else sreg16(0, sext8(lreg8(0)));
		break;
	case 0x99:                                                       /* CWD, CDQ */
		if (o32) sreg32(2, (s32)lreg32(0) >> 31); else sreg16(2, sext16(lreg16(0)) >> 16);
		break;
	case 0xf8: cpu->flags &= ~CF; cpu->cc.mask &= ~CF; break;        /* CLC */
	case 0xf9: cpu->flags |= CF; cpu->cc.mask &= ~CF; break;         /* STC */
	case 0xf5: { int cf = get_CF(cpu); SET_BIT(cpu->flags, !cf, CF); cpu->cc.mask &= ~CF; break; }  /* CMC */
	case 0xfc: cpu->flags &= ~DF; break;                             /* CLD */
	case 0xfd: cpu->flags |= DF; break;                              /* STD */
	case 0xfa: cpu->flags &= ~IF; break;                             /* CLI (real mode: CPL 0) */
	case 0x9b:                                                       /* WAIT: #NM, as WAIT() */
		if ((cpu->cr0 & 0xa) == 0xa)
			return 0;
		break;
	default: break;                                                  /* NOP */
	}
	return 1;
}

/* the block enders: they set next_ip (aux low half: the IP after the instruction) */

/* Jcc: desc = the condition, aux = next | rel << 16 */
JH(jh_jcc)(CPUI386 *cpu, uint32_t d, uint32_t aux, uint32_t x)
{
	int b1 = d;
	COND()
	cpu->next_ip = cond ? (uword)((aux + (aux >> 16)) & 0xffff) : (aux & 0xffff);
	return 1;
}

/* JMP rel: aux = next | rel << 16 */
JH(jh_jmp)(CPUI386 *cpu, uint32_t d, uint32_t aux, uint32_t x)
{
	(void)d;
	cpu->next_ip = (aux + (aux >> 16)) & 0xffff;
	return 1;
}

/* CALL rel16 (aux = next | rel << 16); CALL rel32 under 66h (width 2: x =
 * rel32, a 32-bit push). IP wraps at 64K, as cpu_exec1's. */
JH(jh_call)(CPUI386 *cpu, uint32_t d, uint32_t aux, uint32_t x)
{
	int w = D_W(d) == 2 ? 2 : 1;
	if (!jit_push(cpu, aux & 0xffff, w))
		return 0;
	cpu->next_ip = (aux + (w == 2 ? x : aux >> 16)) & 0xffff;
	return 1;
}

/* RET, RET imm16 (aux = imm) */
JH(jh_ret)(CPUI386 *cpu, uint32_t d, uint32_t aux, uint32_t x)
{
	int w = D_W(d) == 2 ? 2 : 1;
	uword sp = lreg16(4);
	uintptr_t h = JIT_HP(SEG_SS, sp, 1 << w, 0);
	if (!h)
		return 0;
	cpu->next_ip = jit_rdh(h, w) & 0xffff;
	sreg16(4, (sp + (1u << w) + aux) & 0xffff);
	return 1;
}

/* LOOPNE LOOPE LOOP JCXZ (desc = the opcode), aux = next | rel << 16 */
JH(jh_loop)(CPUI386 *cpu, uint32_t d, uint32_t aux, uint32_t x)
{
	uword next = aux & 0xffff, to = (aux + (aux >> 16)) & 0xffff;
	if (d == 0xe3) {
		cpu->next_ip = lreg16(1) == 0 ? to : next;
		return 1;
	}
	sreg16(1, lreg16(1) - 1);
	bool take = lreg16(1) != 0;
	if (d == 0xe1) take = take && get_ZF(cpu);
	else if (d == 0xe0) take = take && !get_ZF(cpu);
	cpu->next_ip = take ? to : next;
	return 1;
}

/* ---- Thumb-2 ---- */

static uint32_t jit_pos;                /* halfword index being written */
static bool jit_full;

static inline void jit_h(uint16_t h)
{
	if (jit_pos < jit_free_end)             /* (beyond it: live code) */
		jit_code[jit_pos++] = h;
	else
		jit_full = true;
}

static void jit_movw(int rd, uint32_t v, bool top)
{
	uint16_t imm = top ? v >> 16 : v & 0xffff;
	jit_h((top ? 0xF2C0 : 0xF240) | ((imm >> 1) & 0x400) | (imm >> 12));
	jit_h(((imm << 4) & 0x7000) | (rd << 8) | (imm & 0xff));
}

static void jit_imm(int rd, uint32_t v)
{
	jit_movw(rd, v, false);
	if (v >> 16)
		jit_movw(rd, v, true);
}

/* frank-dos: the BLs of the block being compiled (to helpers in SRAM: the
 * only code in a block that depends on where it is), for the store */
#define JIT_MAX_RELS 128u
static uint16_t jit_rel_off[JIT_MAX_RELS] __attribute__((section(".psram_bss")));   /* (no SRAM to spare) */
static uint32_t jit_rel_to[JIT_MAX_RELS] __attribute__((section(".psram_bss")));
static unsigned jit_nrel;
static bool jit_rel_bad;                /* (more than JIT_MAX_RELS: not stored) */

/* BL (link) or B.W to a halfword index or an address */
static bool jit_branch(uintptr_t from, uintptr_t to, bool link)
{
	int32_t off = (int32_t)(to - (from + 4));
	if (off < -(1 << 24) || off >= (1 << 24))
		return false;
	if (link) {
		if (jit_nrel < JIT_MAX_RELS) {
			jit_rel_off[jit_nrel] = (uint16_t)jit_pos;
			jit_rel_to[jit_nrel++] = (uint32_t)to;
		} else {
			jit_rel_bad = true;
		}
	}
	uint32_t s = (off >> 24) & 1, i1 = (off >> 23) & 1, i2 = (off >> 22) & 1;
	uint32_t j1 = (!i1) ^ s, j2 = (!i2) ^ s;
	jit_h(0xF000 | (s << 10) | ((off >> 12) & 0x3ff));
	jit_h((link ? 0xD000 : 0x9000) | (j1 << 13) | (j2 << 11) | ((off >> 1) & 0x7ff));
	return true;
}

static inline uintptr_t jit_addr(uint32_t pos) { return (uintptr_t)&jit_code[pos]; }

/* ---- the compiler ---- */

typedef int (*jit_helper)(CPUI386 *, uint32_t, uint32_t, uint32_t);
typedef uint32_t (*jit_block)(CPUI386 *);

static uint32_t jit_exits[JIT_MAX_INSNS + 1];   /* b.w exit sites to patch */
static int jit_nexits;

/* a helper's call and its bail (k: instructions before it; ip: its own IP) */
static bool jit_emit_call(jit_helper h, uint32_t d, uint32_t aux, bool usex, uint32_t x, unsigned k, uword ip,
			  bool ends)
{
	jit_h(0x4620);                          /* mov r0, r4 */
	jit_imm(1, d);
	jit_imm(2, aux);
	if (usex)
		jit_imm(3, x);
#ifdef JIT_TEST_BLX                     /* (cpubench: every call the long way) */
	if (true) {
#else
	if (!jit_branch(jit_addr(jit_pos), (uintptr_t)h & ~1u, true)) {
#endif
		jit_imm(12, (uintptr_t)h | 1);  /* (out of BL's reach: in flash) */
		jit_h(0x47E0);                  /* blx r12 */
	}
	if (ends) {
		/* cbz r0, bail; movs r0, #k + 1; pop {r4, pc} */
		uint32_t cb = jit_pos;
		jit_h(0);
		jit_h(0x2000 | (k + 1));
		jit_h(0xBD10);
		uint32_t off = (jit_pos - cb - 2) * 2;
		jit_code[cb] = 0xB100 | ((off >> 1 & 0x20) << 4) | ((off >> 1 & 0x1f) << 3);
	} else {
		/* cbnz r0, next */
		uint32_t cb = jit_pos;
		jit_h(0);
		jit_movw(1, ip, false);
		jit_h(0x2000 | k);
		jit_exits[jit_nexits++] = jit_pos;
		jit_h(0);
		jit_h(0);
		uint32_t off = (jit_pos - cb - 2) * 2;
		jit_code[cb] = 0xB900 | ((off >> 1 & 0x20) << 4) | ((off >> 1 & 0x1f) << 3);
		return true;
	}
	/* the bail of an ender */
	jit_movw(1, ip, false);
	jit_h(0x2000 | k);
	jit_exits[jit_nexits++] = jit_pos;
	jit_h(0);
	jit_h(0);
	return true;
}

/* ---- native code: register and immediate forms (they can't fault) ---- */

/* a guest register's offset in CPUI386, width w (byte: AL..BH) */
static inline uint32_t jit_goff(int r, int w)
{
	return w == 0 && r > 3 ? (r - 4) * 4 + 1 : r * 4;
}

/* ARM rt = guest register r of width w: zero-extended (ldrb/ldrh) or
 * sign-extended; a dword is ldr */
static void jit_ldg(int rt, int r, int w, bool sx)
{
	uint32_t o = jit_goff(r, w);
	if (w == 2)
		jit_h(0x6800 | (o >> 2) << 6 | 4 << 3 | rt);    /* ldr rt, [r4, #o] */
	else if (sx) {
		jit_h(w == 0 ? 0xF994 : 0xF9B4);        /* ldrsb.w / ldrsh.w rt, [r4, #o] */
		jit_h(rt << 12 | o);
	} else
		jit_h((w == 0 ? 0x7800 | o << 6 : 0x8800 | (o >> 1) << 6) | 4 << 3 | rt);
}

static void jit_stg(int rt, int r, int w)
{
	uint32_t o = jit_goff(r, w);
	jit_h((w == 2 ? 0x6000 | (o >> 2) << 6 : w == 0 ? 0x7000 | o << 6 : 0x8000 | (o >> 1) << 6) | 4 << 3 | rt);
}

_Static_assert(offsetof(CPUI386, cc.mask) < 4096, "str.w's imm12");

/* str.w rt, [r4, #o] */
static void jit_str(int rt, uint32_t o)
{
	jit_h(0xF8C4);
	jit_h(rt << 12 | o);
}

/* ldr.w rt, [r4, #o] */
static void jit_ldr(int rt, uint32_t o)
{
	jit_h(0xF8D4);
	jit_h(rt << 12 | o);
}

/* MOV between registers (src, dst) */
static void jit_nat_mov(int dst, int src, int w)
{
	jit_ldg(0, src, w, false);
	jit_stg(0, dst, w);
}

static void jit_nat_mov_ri(int dst, uint32_t imm, int w)
{
	if (w == 0)
		jit_h(0x2000 | (imm & 0xff));   /* movs r0, #imm */
	else if (w == 1)
		jit_movw(0, imm & 0xffff, false);
	else
		jit_imm(0, imm);
	jit_stg(0, dst, w);
}

/* what the lazy flags hold at this point of the block being compiled: set
 * by an instruction of it whose kind is known, a Jcc after it tests them
 * natively. SUB: cc.src1 - cc.src2 (SUB, CMP); LOGIC: cc.dst with CF = OF = 0
 * (AND, OR, XOR, TEST); ADD: cc.op CC_ADD; INCDEC: CF in cpu->flags; DST:
 * only cc.dst's ZF and SF (ADC, SBB). A Jcc reads ADD and INCDEC as DST. */
enum { JCC_NONE, JCC_SUB, JCC_LOGIC, JCC_DST, JCC_ADD, JCC_INCDEC };
static int jit_cck;

static int jit_cck_of(int op)
{
	return op == 5 || op == 7 ? JCC_SUB : op == 1 || op == 4 || op == 6 || op == 8 ? JCC_LOGIC :
	       op == 0 ? JCC_ADD : JCC_DST;
}

/* register dst = dst OP src (or the immediate), op as jh_alu's (8: TEST);
 * ADC/SBB (they read CF) are left to the helper */
static int jit_nat_alu_ops(int op, int w);

/* can ADC/SBB be native here (CF from the known kind of the last flag setter)? */
static inline bool jit_cf_known(void)
{
#ifdef JIT_NO_NATADC
	return false;
#endif
	return jit_cck == JCC_LOGIC || jit_cck == JCC_SUB || jit_cck == JCC_ADD || jit_cck == JCC_INCDEC;
}

static bool jit_nat_alu(int op, int w, int dst, int src, bool imm, uint32_t ival)
{
	if ((op == 2 || op == 3) && !jit_cf_known())
		return false;
	jit_ldg(0, dst, w, true);
	if (imm)
		jit_imm(1, (uint32_t)jit_sx(ival, w));
	else
		jit_ldg(1, src, w, true);
	int res = jit_nat_alu_ops(op, w);
	if (op < 7)
		jit_stg(res, dst, w);
	return true;
}

/* r0 OP r1 (both sign-extended from width w): the lazy flags, jit_cck; the
 * result's register (0 or 2). Not ADC/SBB. */
static int jit_nat_alu_ops(int op, int w)
{
	int res, cc;
	if (op == 2 || op == 3) {
		/* ADC/SBB (ACOP_helper): r2 = CF as get_CF makes it (jit_cf_known) */
		const uint32_t fl = offsetof(CPUI386, flags);
		switch (jit_cck) {
		case JCC_LOGIC:
			jit_h(0x2200);                          /* movs r2, #0 */
			break;
		case JCC_INCDEC:
			jit_h(0x6800 | (fl >> 2) << 6 | 4 << 3 | 2);    /* ldr r2, [r4, #flags] */
			jit_h(0xF002); jit_h(0x0201);           /* and r2, r2, #CF */
			break;
		default:                                /* SUB: src1 < src2; ADD: dst < src2 */
			jit_ldr(2, jit_cck == JCC_SUB ? offsetof(CPUI386, cc.src1) : offsetof(CPUI386, cc.dst));
			jit_ldr(3, offsetof(CPUI386, cc.src2));
			jit_h(0x429A);                          /* cmp r2, r3 */
			jit_h(0xBF34);                          /* ite lo */
			jit_h(0x2201);                          /* movlo r2, #1 */
			jit_h(0x2200);                          /* movhs r2, #0 */
			break;
		}
		jit_h(op == 2 ? 0x1843 : 0x1A43);       /* adds / subs r3, r0, r1 */
		jit_h(op == 2 ? 0x189B : 0x1A9B);       /* adds / subs r3, r3, r2 */
		if (w < 2)
			jit_h(w == 0 ? 0xB25B : 0xB21B);        /* sxtb / sxth r3, r3 */
		jit_str(0, offsetof(CPUI386, cc.src1));
		jit_str(1, offsetof(CPUI386, cc.src2));
		jit_str(3, offsetof(CPUI386, cc.dst));
		/* cc.op: CF ? CC_ADC : CC_ADD, CF ? CC_SBB : CC_SUB */
		_Static_assert(CC_ADD == CC_ADC + 1 && CC_SUB == CC_SBB + 1, "rsb");
		jit_h(0xF1C2); jit_h(0x0200 | (op == 2 ? CC_ADD : CC_SUB));    /* rsb r2, r2, #op */
		jit_str(2, offsetof(CPUI386, cc.op));
		jit_movw(2, CF | PF | AF | ZF | SF | OF, false);
		jit_str(2, offsetof(CPUI386, cc.mask));
		jit_cck = JCC_DST;
		return 3;
	}
	uint32_t mask;
	switch (op) {
	case 1: jit_h(0x4308); res = 0; cc = CC_OR; break;      /* orrs r0, r1 */
	case 4: case 8: jit_h(0x4008); res = 0; cc = CC_AND; break;     /* ands r0, r1 */
	case 6: jit_h(0x4048); res = 0; cc = CC_XOR; break;     /* eors r0, r1 */
	case 0: jit_h(0x1842); res = 2; cc = CC_ADD; break;     /* adds r2, r0, r1 */
	default: jit_h(0x1A42); res = 2; cc = CC_SUB; break;    /* subs r2, r0, r1 */
	}
	if (res == 2) {
		if (w < 2)
			jit_h(w == 0 ? 0xB252 : 0xB212);        /* sxtb / sxth r2, r2 */
		jit_str(0, offsetof(CPUI386, cc.src1));
		jit_str(1, offsetof(CPUI386, cc.src2));
		mask = CF | PF | AF | ZF | SF | OF;
	} else
		mask = CF | PF | ZF | SF | OF;
	jit_str(res, offsetof(CPUI386, cc.dst));
	jit_h(0x2300 | cc);                     /* movs r3, #cc */
	jit_str(3, offsetof(CPUI386, cc.op));
	jit_movw(3, mask, false);
	jit_str(3, offsetof(CPUI386, cc.mask));
	jit_cck = jit_cck_of(op);
	return res;
}

static void jit_nat_mem_begin(uint32_t d, uint32_t x, int size, bool wr, unsigned k, uword ip);
static void jit_ld12(int rt, int w, bool sx);
static void jit_st12(int rt, int w);

/* SHL/SHR/SAR (group 2 /4 /5 /7) of register r, or of memory (mem: as
 * jit_nat_incdec), by a constant y, 1..31 (SHL: y <= the width): SHL_helper,
 * SHR_helper, SAR_helper. false: the helper. */
static bool jit_nat_shift(int op, int y, int r, int w, bool mem, uint32_t d, uint32_t x, unsigned k, uword ip)
{
	int bits = 8 << w;
#ifdef JIT_NO_NATSHIFT
	return false;
#endif
	if (y < 1 || y > 31 || (op != 4 && op != 5 && op != 7) || (op == 4 && y > bits))
		return false;
	if (mem) {
		jit_nat_mem_begin(d, x, 1 << w, true, k, ip);
		jit_ld12(0, w, op == 7);
	} else
		jit_ldg(0, r, w, op == 7);
	int lsb = op == 4 ? bits - y : y - 1;
	jit_h(0xF3C0); jit_h((lsb >> 2) << 12 | 2 << 8 | (lsb & 3) << 6);      /* ubfx r2, r0, #lsb, #1 */
	if (op == 4) {
		jit_h(0x0000 | y << 6 | 0 << 3 | 1);           /* lsls r1, r0, #y */
		if (w < 2)
			jit_h(w == 0 ? 0xB249 : 0xB209);        /* sxtb / sxth r1, r1 */
	} else if (op == 5) {
		if (w < 2) {
			jit_h(w == 0 ? 0xB243 : 0xB203);        /* sxtb / sxth r3, r0 */
			jit_str(3, offsetof(CPUI386, cc.src1));
		} else
			jit_str(0, offsetof(CPUI386, cc.src1));
		jit_h(0x0800 | y << 6 | 0 << 3 | 1);           /* lsrs r1, r0, #y */
	} else
		jit_h(0x1000 | y << 6 | 0 << 3 | 1);           /* asrs r1, r0, #y */
	jit_str(1, offsetof(CPUI386, cc.dst));
	jit_str(2, offsetof(CPUI386, cc.dst2));
	jit_h(0x2300 | (op == 4 ? CC_SHL : op == 5 ? CC_SHR : CC_SAR));
	jit_str(3, offsetof(CPUI386, cc.op));
	jit_movw(3, CF | PF | ZF | SF | OF, false);
	jit_str(3, offsetof(CPUI386, cc.mask));
	if (mem)
		jit_st12(1, w);
	else
		jit_stg(1, r, w);
	jit_cck = JCC_NONE;
	return true;
}

/* INC/DEC (INCDEC_helper) of register r, or of memory (mem: desc d, disp x;
 * k, ip as jit_nat_mem_begin's), width w, when the kind of what set the
 * flags last is known (jit_cck): CF is saved into cpu->flags as get_CF would
 * make it; false: the helper */
static bool jit_nat_incdec(int r, bool dec, int w, bool mem, uint32_t d, uint32_t x, unsigned k, uword ip)
{
#ifdef JIT_NO_CCK
	jit_cck = JCC_NONE;
#endif
	const uint32_t fl = offsetof(CPUI386, flags);
	if (jit_cck != JCC_INCDEC && jit_cck != JCC_LOGIC && jit_cck != JCC_SUB && jit_cck != JCC_ADD)
		return false;
	if (mem)
		jit_nat_mem_begin(d, x, 1 << w, true, k, ip);          /* (r12; before anything changes) */
	switch (jit_cck) {
	case JCC_INCDEC:                        /* (CF is in cpu->flags already) */
		break;
	case JCC_LOGIC:
		jit_h(0x6800 | (fl >> 2) << 6 | 4 << 3 | 1);    /* ldr r1, [r4, #flags] */
		jit_h(0xF021); jit_h(0x0101);                   /* bic r1, r1, #CF */
		jit_h(0x6000 | (fl >> 2) << 6 | 4 << 3 | 1);    /* str r1, [r4, #flags] */
		break;
	case JCC_SUB:                           /* CF = src1 < src2 */
	case JCC_ADD:                           /* CF = dst < src2 */
		jit_ldr(2, jit_cck == JCC_SUB ? offsetof(CPUI386, cc.src1) : offsetof(CPUI386, cc.dst));
		jit_ldr(3, offsetof(CPUI386, cc.src2));
		jit_h(0x6800 | (fl >> 2) << 6 | 4 << 3 | 1);
		jit_h(0xF021); jit_h(0x0101);
		jit_h(0x429A);                                  /* cmp r2, r3 */
		jit_h(0xBF38);                                  /* it lo */
		jit_h(0xF041); jit_h(0x0101);                   /* orrlo r1, r1, #CF */
		jit_h(0x6000 | (fl >> 2) << 6 | 4 << 3 | 1);
		break;
	default:
		return false;
	}
	_Static_assert(CF == 1, "bic/orr #1");
	if (mem)
		jit_ld12(0, w, true);
	else
		jit_ldg(0, r, w, true);
	jit_h(dec ? 0x3801 : 0x3001);           /* subs / adds r0, #1 */
	if (w < 2)
		jit_h(w == 0 ? 0xB240 : 0xB200);        /* sxtb / sxth r0, r0 */
	jit_str(0, offsetof(CPUI386, cc.dst));
	static const uint8_t ccs[2][3] = { { CC_INC8, CC_INC16, CC_INC32 }, { CC_DEC8, CC_DEC16, CC_DEC32 } };
	jit_h(0x2300 | ccs[dec][w]);
	jit_str(3, offsetof(CPUI386, cc.op));
	jit_movw(3, PF | AF | ZF | SF | OF, false);
	jit_str(3, offsetof(CPUI386, cc.mask));
	if (mem)
		jit_st12(0, w);
	else
		jit_stg(0, r, w);
	jit_cck = JCC_INCDEC;
	return true;
}

/* the enders, natively: r0 = the count, r1 = the next IP, then the block's
 * exit (it follows) stores it. A condition c (ARM) picks r12 = the target. */
static void jit_nat_go(unsigned k, uword next, uword to)
{
	jit_h(0x2000 | (k + 1));                /* movs r0, #k + 1 */
	jit_movw(1, next, false);
	jit_movw(12, to, false);
}

static void jit_nat_take_if(int c)
{
	jit_h(0xBF08 | c << 4);                 /* it c */
	jit_h(0x4661);                          /* mov r1, r12 */
}

/* Jcc on the flags kind jit_cck, x86 condition cc: 1 native, the block goes
 * on (the taken path leaves it); 2 native, the block ends (always taken); 0
 * the helper's */
static int jit_nat_jcc(int cc, unsigned k, uword next, uword to)
{
	(void)next;
#ifdef JIT_NO_CCK
	jit_cck = JCC_NONE;
#endif
	static const int8_t sub[16] = { -1, -1, 3, 2, 0, 1, 9, 8, 4, 5, -1, -1, 11, 10, 13, 12 };
	/* (CF = 0: B never, AE always; BE = E; A = NE) */
	static const int8_t logic[16] = { -1, -1, 16, 17, 0, 1, 0, 1, 4, 5, -1, -1, 11, 10, 13, 12 };
	static const int8_t dst[16] = { -1, -1, -1, -1, 0, 1, -1, -1, 4, 5, -1, -1, -1, -1, -1, -1 };
	int c = jit_cck == JCC_SUB ? sub[cc] : jit_cck == JCC_LOGIC ? logic[cc] :
		jit_cck == JCC_DST || jit_cck == JCC_ADD || jit_cck == JCC_INCDEC ? dst[cc] : -1;
	if (c < 0)
		return 0;
	if (c == 16)                            /* never taken: the block goes on */
		return 1;
	if (c == 17) {                          /* always: a jump */
		jit_h(0x2000 | (k + 1));
		jit_movw(1, to, false);
		return 2;
	}
	if (jit_cck == JCC_SUB && c != 4 && c != 5) {
		jit_ldr(2, offsetof(CPUI386, cc.src1));
		jit_ldr(3, offsetof(CPUI386, cc.src2));
		jit_h(0x429A);                  /* cmp r2, r3 */
	} else {
		jit_ldr(2, offsetof(CPUI386, cc.dst));
		jit_h(0x2A00);                  /* cmp r2, #0 */
	}
	/* not taken: on in the block; taken: out of it to the target */
	uint32_t bcc = jit_pos;
	jit_h(0xD000 | (c ^ 1) << 8);           /* b<!c> on (patched) */
	jit_h(0x2000 | (k + 1));                /* movs r0, #k + 1 */
	jit_movw(1, to, false);
	jit_exits[jit_nexits++] = jit_pos;
	jit_h(0);                               /* b.w exit (patched) */
	jit_h(0);
	jit_code[bcc] |= (jit_pos - bcc - 2) & 0xff;
	return 1;
}

/* LOOP (e2), JCXZ (e3) */
static void jit_nat_loop(bool jcxz, unsigned k, uword next, uword to)
{
	jit_nat_go(k, next, to);
	jit_h(0x88A2);                          /* ldrh r2, [r4, #4] (CX) */
	if (jcxz)
		jit_h(0x2A00);                  /* cmp r2, #0 */
	else {
		jit_h(0x3A01);                  /* subs r2, #1 */
		jit_h(0x80A2);                  /* strh r2, [r4, #4] */
		jit_h(0x0412);                  /* lsls r2, r2, #16 */
	}
	jit_nat_take_if(jcxz ? 0 : 1);
}

/* ---- shared routines a block calls (code size: the cache is small) ----
 * Each can end the block itself: it pops its own frame and the block's
 * (push {r4, lr} at its start) and returns to jit_enter with r0 = the
 * instructions done; next_ip is set. */


/* (the TLB entry's size: tlb_entry's pad, i386.h) */
#if UINTPTR_MAX == 0xffffffffu && (FX_KERNEL || !defined(PICO_ON_DEVICE))
#define JIT_TLB_28 ""
_Static_assert(sizeof(struct tlb_entry) == 32, "jit_mem_rd's index");
#else
#define JIT_TLB_28 "sub r12, r12, lr, lsl #2\n\t"
_Static_assert(sizeof(struct tlb_entry) == 28, "jit_mem_rd's index");
#endif


/* the host address of a memory operand (r1 = offset, r2 = desc), or 0: the
 * slow path couldn't (an I/O page, across pages, a store into compiled code,
 * an exception to come) */
static uintptr_t JIT_RAM(jit_mem_c)(CPUI386 *cpu, uword off, uint32_t desc)
{
	int sg = (desc >> 12) & 7, size = ((desc >> 8) & 7) + 1;
	bool wr = desc >> 15;
	uword lin = cpu->seg[sg].base + off, lim = cpu->seg[sg].limit;
	struct tlb_entry *e = &cpu->tlb.tab[(lin >> 12) % tlb_size];
	if (likely((wr ? e->wtag : e->rtag) == (lin >> 12) && (lin & 0xfff) <= 0x1000u - size && off <= lim &&
		   lim - off >= (uword)size - 1))
		return e->hbase + lin;
	OptAddr m;
	if (!jit_mem_slow(cpu, &m, wr ? 2 : 1, sg, off, size) || m.res != ADDR_OK1)
		return 0;
	return (uintptr_t)cpu->phys_mem + m.addr1;
}

static int jit_step_c(CPUI386 *cpu, uint32_t packed);

/* r1 = offset, r2 = desc, r3 = IP | k << 16 | length << 22 | resume << 26
 * -> r0 = the host address: the TLB's hit here (JIT_AT's checks), else
 * jit_mem_c. If that can't (an I/O page such as VGA's, across pages, a store
 * into compiled code), the instruction is one cpu_exec1 step and the block
 * goes on at its return address + resume halfwords (the next instruction's
 * code), unless the step ends it; resume 0: the block ends before it (k
 * done, next_ip = IP). */
#define JIT_MEM_TRAMP(name, tag) \
__attribute__((naked, noinline)) static void JIT_RAM(name)(void) \
{ \
	__asm volatile( \
		"push {r1, r2, r3, lr}\n\t" \
		"uxtb r0, r2\n\t" \
		"add r0, r4\n\t"                       /* &seg.base */ \
		"ldr r12, [r0, #4]\n\t"                /* limit */ \
		"ubfx lr, r2, #8, #3\n\t" \
		"adds lr, r1, lr\n\t"                  /* the last byte's offset */ \
		"bcs 2f\n\t" \
		"cmp lr, r12\n\t" \
		"bhi 2f\n\t" \
		"ldr r0, [r0]\n\t" \
		"add r0, r1\n\t"                       /* linear */ \
		"ldr r12, [r4, %[tab]]\n\t" \
		"ubfx lr, r0, #12, #9\n\t" \
		"add r12, r12, lr, lsl #5\n\t" \
		JIT_TLB_28 \
		"ldr lr, [r12, %[tg]]\n\t" \
		"cmp lr, r0, lsr #12\n\t" \
		"bne 2f\n\t" \
		"ubfx lr, r0, #0, #12\n\t" \
		"ubfx r3, r2, #8, #3\n\t" \
		"add lr, r3\n\t" \
		"lsrs lr, lr, #12\n\t"                 /* across the page */ \
		"bne 2f\n\t" \
		"ldr lr, [r12, %[hb]]\n\t" \
		"add r0, lr\n\t" \
		"pop {r1, r2, r3, pc}\n" \
		"2:\n\t" \
		"mov r0, r4\n\t" \
		"ldrd r1, r2, [sp]\n\t" \
		"bl %c[fn]\n\t" \
		"cbz r0, 1f\n\t" \
		"pop {r1, r2, r3, pc}\n" \
		"1:\n\t"                              /* the slow path couldn't (I/O memory...) */ \
		"ldr r3, [sp, #8]\n\t" \
		"lsrs r12, r3, #26\n\t"                /* where the next instruction's code is */ \
		"beq 3f\n\t" \
		"uxth r1, r3\n\t"                      /* the instruction by jit_step_c */ \
		"ubfx r0, r3, #22, #4\n\t" \
		"orr r1, r1, r0, lsl #16\n\t" \
		"ubfx r0, r3, #16, #6\n\t" \
		"orr r1, r1, r0, lsl #24\n\t" \
		"mov r0, r4\n\t" \
		"bl %c[step]\n\t" \
		"cmp r0, #1\n\t" \
		"bne 4f\n\t" \
		"ldrd r3, r12, [sp, #8]\n\t"          /* done: on in the block after it */ \
		"lsrs r0, r3, #26\n\t" \
		"add r12, r12, r0, lsl #1\n\t" \
		"add sp, #16\n\t" \
		"bx r12\n" \
		"4:\n\t"                              /* it ended the block, or it is undone */ \
		"ldr r3, [sp, #8]\n\t" \
		"ubfx r1, r3, #16, #6\n\t" \
		"cmp r0, #0\n\t" \
		"ite eq\n\t" \
		"moveq r0, r1\n\t" \
		"addne r0, r1, #1\n\t" \
		"add sp, #16\n\t" \
		"pop {r4, pc}\n" \
		"3:\n\t"                              /* (no resume point: the block ends before it) */ \
		"ldr r3, [sp, #8]\n\t" \
		"add sp, #16\n\t" \
		"uxth r1, r3\n\t" \
		"ubfx r0, r3, #16, #6\n\t" \
		"str r1, [r4, %[nip]]\n\t" \
		"pop {r4, pc}\n\t" \
		:: [fn] "i" (jit_mem_c), [step] "i" (jit_step_c), [nip] "i" (offsetof(CPUI386, next_ip)), \
		   [tab] "i" (offsetof(CPUI386, tlb.tab)), [tg] "i" (tag), \
		   [hb] "i" (offsetof(struct tlb_entry, hbase))); \
}
_Static_assert(offsetof(__typeof__(((CPUI386 *)0)->seg[0]), limit) ==
	       offsetof(__typeof__(((CPUI386 *)0)->seg[0]), base) + 4, "[seg.base + 4]");
_Static_assert(JIT_SEGOFF(SEG_GS, base) < 256, "uxtb");
JIT_MEM_TRAMP(jit_mem_rd, offsetof(struct tlb_entry, rtag))
JIT_MEM_TRAMP(jit_mem_wr, offsetof(struct tlb_entry, wtag))

static int jit_step_c(CPUI386 *cpu, uint32_t packed);

/* r1 = IP | length << 16 | k << 24: the instruction by cpu_exec1 (jh_step);
 * back into the block, or the block ends (k + 1 done, or k if undone) */
__attribute__((naked, noinline)) static void JIT_RAM(jit_step_tramp)(void)
{
	__asm volatile(
		"push {r1, lr}\n\t"
		"mov r0, r4\n\t"
		"bl %c[fn]\n\t"
		"cmp r0, #1\n\t"
		"it eq\n\t"
		"popeq {r1, pc}\n\t"
		"pop {r1, r12}\n\t"
		"lsrs r1, r1, #24\n\t"
		"cmp r0, #0\n\t"
		"ite eq\n\t"
		"moveq r0, r1\n\t"
		"addne r0, r1, #1\n\t"
		"pop {r4, pc}\n\t"
		:: [fn] "i" (jit_step_c));
}

/* bl to a routine (blx r12 when out of reach) */
static void jit_bl(uintptr_t fn)
{
	if (!jit_branch(jit_addr(jit_pos), fn & ~1u, true)) {
		jit_imm(12, fn | 1);
		jit_h(0x47E0);                  /* blx r12 */
	}
}

/* ---- native memory operands ----
 * The E operand's offset inline, its host address from jit_mem_tramp, the
 * access through it. */

_Static_assert(offsetof(CPUI386, tlb.tab) < 4096, "ldr.w's imm12");
_Static_assert(tlb_size == 512, "ubfx #12, #9");
_Static_assert(sizeof(struct tlb_entry) == 28 || sizeof(struct tlb_entry) == 32, "the entry index");

/* the segment an E operand uses (jit_ea's choice) */
static int jit_ea_seg(uint32_t d)
{
	int rm = D_RM(d), mod = D_MOD(d), s = D_SEG(d);
	if (s != 7)
		return s;
	if (!(d & D_A32))
		return rm == 2 || rm == 3 || (mod != 0 && rm == 6) ? SEG_SS : SEG_DS;
	if (rm == 4) {
		int base = D_SIB(d) & 7;
		return !(base == 5 && mod == 0) && (base == 4 || base == 5) ? SEG_SS : SEG_DS;
	}
	return rm == 5 && mod != 0 ? SEG_SS : SEG_DS;
}

/* r1 = the E operand's offset (desc d, disp x) */
static void jit_nat_off(uint32_t d, uint32_t x)
{
	int rm = D_RM(d), mod = D_MOD(d);
	if (!(d & D_A32)) {
		static const int8_t b1[8] = { 3, 3, 5, 5, 6, 7, 5, 3 }, b2[8] = { 6, 7, 6, 7, -1, -1, -1, -1 };
		uint32_t disp = D_DISP(d);
		if (mod == 0 && rm == 6) {
			jit_movw(1, disp, false);
			return;
		}
		jit_h(0x8800 | (b1[rm] * 2) << 6 | 4 << 3 | 1);         /* ldrh r1, [r4, #b1] */
		if (b2[rm] >= 0) {
			jit_h(0x8800 | (b2[rm] * 2) << 6 | 4 << 3 | 2);     /* ldrh r2, [r4, #b2] */
			jit_h(0x1889);                                  /* adds r1, r1, r2 */
		}
		if (disp && disp < 256)
			jit_h(0x3100 | disp);                           /* adds r1, #disp */
		else if (disp) {
			jit_movw(2, disp, false);
			jit_h(0x1889);
		}
		jit_h(0xB289);                                          /* uxth r1, r1 */
		return;
	}
	uint32_t disp = d & D_XDISP ? x : 0;
	if (rm == 4) {
		int sib = D_SIB(d), base = sib & 7, idx = (sib >> 3) & 7, sc = sib >> 6;
		bool hb = !(base == 5 && mod == 0);
		if (hb)
			jit_h(0x6800 | base << 6 | 4 << 3 | 1);         /* ldr r1, [r4, #base * 4] */
		if (idx != 4) {
			jit_h(0x6800 | idx << 6 | 4 << 3 | 2);          /* ldr r2, [r4, #idx * 4] */
			if (hb) {
				jit_h(0xEB01);                          /* add.w r1, r1, r2, lsl #sc */
				jit_h((sc >> 2) << 12 | 1 << 8 | (sc & 3) << 6 | 2);
			} else
				jit_h(sc << 6 | 2 << 3 | 1);            /* lsls r1, r2, #sc */
		} else if (!hb)
			jit_h(0x2100);                                  /* movs r1, #0 */
	} else if (rm == 5 && mod == 0)
		jit_h(0x2100);
	else
		jit_h(0x6800 | rm << 6 | 4 << 3 | 1);                   /* ldr r1, [r4, #rm * 4] */
	if (disp) {
		jit_imm(2, disp);
		jit_h(0x1889);
	}
}

/* rt = [r12], width w: zero- or sign-extended */
static void jit_ld12(int rt, int w, bool sx)
{
	jit_h(w == 2 ? 0xF8DC : w == 1 ? (sx ? 0xF9BC : 0xF8BC) : (sx ? 0xF99C : 0xF89C));
	jit_h(rt << 12);
}

static void jit_st12(int rt, int w)
{
	jit_h(w == 2 ? 0xF8CC : w == 1 ? 0xF8AC : 0xF88C);
	jit_h(rt << 12);
}

/* an instruction's native memory operand: r12 = its host address (k: the
 * instructions before it, ip its own; a miss the slow path can't take ends
 * the block before it, in jit_mem_tramp) */
/* the trampoline calls of the instruction being compiled: their r3's movt
 * (resume and length patched in when its code is done, jit_mem_resume) */
static uint32_t jit_r3pos[2], jit_lrpos[2];
static int jit_nr3;

static void jit_nat_addr(int sg, int size, bool wr, unsigned k, uword ip)
{
	jit_movw(2, JIT_MDESC(sg, size, wr), false);
	if (jit_nr3 < 2) {
		jit_r3pos[jit_nr3] = jit_pos + 2;
	}
	jit_movw(3, ip, false);
	jit_movw(3, k << 16, true);             /* (movt: resume, length patched in) */
	jit_bl((uintptr_t)(wr ? jit_mem_wr : jit_mem_rd));
	if (jit_nr3 < 2)
		jit_lrpos[jit_nr3++] = jit_pos;
	jit_h(0x4684);                          /* mov r12, r0 */
}

/* the instruction (n bytes, k-th, at IP ip) is compiled: its trampoline calls
 * learn where its code ends */
static void jit_mem_resume(int n, unsigned k)
{
	uint32_t save = jit_pos;
	for (int i = 0; i < jit_nr3; i++) {
		uint32_t res = save - jit_lrpos[i];
		if (res > 63)
			res = 0;                /* (too far: the block ends there instead) */
		jit_pos = jit_r3pos[i];
		jit_movw(3, (k | n << 6 | res << 10) << 16, true);
	}
	jit_pos = save;
	jit_nr3 = 0;
}

static void jit_nat_mem_begin(uint32_t d, uint32_t x, int size, bool wr, unsigned k, uword ip)
{
	jit_nat_off(d, x);
	jit_nat_addr(jit_ea_seg(d), size, wr, k, ip);
}

/* r12 = SS:SP - size (push) or SS:SP (pop), SP unchanged yet */
static void jit_nat_stack(bool push, int size, unsigned k, uword ip)
{
	jit_h(0x8800 | (4 * 2) << 6 | 4 << 3 | 1);             /* ldrh r1, [r4, #SP] */
	if (push) {
		jit_h(0x3900 | size);                           /* subs r1, #size */
		jit_h(0xB289);                                  /* uxth r1, r1 */
	}
	jit_nat_addr(SEG_SS, size, push, k, ip);
}

/* SP += delta (16 bits) */
static void jit_nat_sp(int delta)
{
	jit_h(0x8800 | (4 * 2) << 6 | 4 << 3 | 1);             /* ldrh r1, [r4, #SP] */
	jit_h(delta < 0 ? 0x3900 | -delta : 0x3100 | delta);   /* subs / adds r1, #|delta| */
	jit_h(0x8000 | (4 * 2) << 6 | 4 << 3 | 1);             /* strh r1, [r4, #SP] */
}

/* segment register sg (not CS, SS) = r0, as set_seg in real mode */
static void jit_nat_setseg(int sg)
{
	jit_h(0xB280);                                          /* uxth r0, r0 */
	jit_str(0, JIT_SEGOFF(sg, sel));
	jit_h(0x0101);                                          /* lsls r1, r0, #4 */
	jit_str(1, JIT_SEGOFF(sg, base));
	jit_movw(2, 0xffff, false);
	jit_str(2, JIT_SEGOFF(sg, limit));
	jit_h(0x2300);                                          /* movs r3, #0 */
	jit_str(3, JIT_SEGOFF(sg, flags));
}

/* ---- the instructions the compiler doesn't take: one cpu_exec1 step each ---- */

/* the length of the instruction at p in 16-bit code; 0: not known here */
static int jit_insn_len(const u8 *p)
{
	int n = 0;
	bool o32 = false, a32 = false;
	for (;;) {
		u8 c = p[n];
		if (c == 0x26 || c == 0x2e || c == 0x36 || c == 0x3e || c == 0x64 || c == 0x65 || c == 0xf0 ||
		    c == 0xf2 || c == 0xf3) n++;
		else if (c == 0x66) o32 = true, n++;
		else if (c == 0x67) a32 = true, n++;
		else break;
		if (n > 6)
			return 0;
	}
	int iv = o32 ? 4 : 2, imm = 0;
	bool m = false;
	u8 b = p[n++];
	if (b == 0x0f) {
		static const uint8_t nomodrm[] = { 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0e, 0x30, 0x31, 0x32,
						   0x33, 0x34, 0x35, 0x37, 0x77, 0xa0, 0xa1, 0xa2, 0xa8, 0xa9, 0xaa };
		u8 c = p[n++];
		if (c == 0x38 || c == 0x3a || c == 0x0f)
			return 0;
		if (c >= 0x80 && c <= 0x8f)
			return n + iv;
		if (c >= 0xc8 && c <= 0xcf)
			return n;
		for (unsigned i = 0; i < sizeof nomodrm; i++)
			if (c == nomodrm[i])
				return n;
		m = true;
		if (c == 0xa4 || c == 0xac || c == 0xba || (c >= 0x70 && c <= 0x73) || c == 0xc2 || (c >= 0xc4 && c <= 0xc6))
			imm = 1;
	} else {
		m = (b < 0x40 && (b & 7) < 4) || b == 0x62 || b == 0x63 || b == 0x69 || b == 0x6b ||
		    (b >= 0x80 && b <= 0x8f) || b == 0xc0 || b == 0xc1 || (b >= 0xc4 && b <= 0xc7) ||
		    (b >= 0xd0 && b <= 0xd3) || (b >= 0xd8 && b <= 0xdf) || b == 0xf6 || b == 0xf7 || b == 0xfe || b == 0xff;
		if (b < 0x40 && (b & 7) == 4) imm = 1;
		else if (b < 0x40 && (b & 7) == 5) imm = iv;
		else if (b == 0x68 || b == 0x69 || b == 0x81 || b == 0xa9 || b == 0xc7 || b == 0xe8 || b == 0xe9 ||
			 (b >= 0xb8 && b <= 0xbf)) imm = iv;
		else if (b == 0x6a || b == 0x6b || (b >= 0x70 && b <= 0x7f) || b == 0x80 || b == 0x82 || b == 0x83 ||
			 b == 0xa8 || (b >= 0xb0 && b <= 0xb7) || b == 0xc0 || b == 0xc1 || b == 0xc6 || b == 0xcd ||
			 b == 0xd4 || b == 0xd5 || (b >= 0xe0 && b <= 0xe7) || b == 0xeb) imm = 1;
		else if (b == 0xc2 || b == 0xca) imm = 2;
		else if (b == 0xc8) imm = 3;
		else if (b == 0x9a || b == 0xea) imm = 2 + iv;
		else if (b >= 0xa0 && b <= 0xa3) imm = a32 ? 4 : 2;
		else if ((b == 0xf6 || b == 0xf7) && ((p[n] >> 3) & 7) < 2) imm = b == 0xf6 ? 1 : iv;
	}
	if (m) {
		u8 r = p[n++];
		int mod = r >> 6, rm = r & 7;
		if (!a32) {
			if (mod == 1) n += 1;
			else if (mod == 2 || (mod == 0 && rm == 6)) n += 2;
		} else if (mod != 3) {
			if (rm == 4 && (p[n++] & 7) == 5 && mod == 0)
				n += 4;
			if (mod == 1) n += 1;
			else if (mod == 2 || (mod == 0 && rm == 5)) n += 4;
		}
	}
	n += imm;
	return n <= 15 ? n : 0;
}

static bool jit_stepping;               /* (jit_enter: no blocks inside a step) */
static bool jit_step_end;               /* a step ended its block: jit_enter checks the CPU's state */

/* the instruction at IP (packed: IP | length << 16 | k << 24) by cpu_exec1
 * alone: 1 done, the block goes on; 2 done, the block ends there (it
 * branched, faulted into a handler, changed CS or the mode, let an interrupt
 * in or halted, or dropped compiled code); 0 it raised an exception: undone
 * (next_ip back at it), the interpreter runs it again and takes it */
static int JIT_RAM(jit_step_c)(CPUI386 *cpu, uint32_t packed)
{
	uword ip = packed & 0xffff, len = (packed >> 16) & 0xff;
	uword csb = cpu->seg[SEG_CS].base;
	uint32_t drops = jit_drops;
	long cyc = cpu->cycle;
	cpu->next_ip = ip;
	jit_stepping = true;
#if JIT_PROF
	jit_prof_instep = true;
#endif
	bool ok = cpu_exec1(cpu, 1);
	jit_stepping = false;
#if JIT_PROF
	jit_prof_instep = false;
#endif
	cpu->cycle = cyc;                       /* (the block counts it) */
	if (!ok) {
		cpu->next_ip = ip;
		return 0;
	}
	if (cpu->next_ip != ((ip + len) & 0xffff) || cpu->seg[SEG_CS].base != csb || (cpu->cr0 & 1) || !cpu->code16 ||
	    cpu->halt || cpu->stop || (cpu->intr && (cpu->flags & IF)) || jit_drops != drops) {
		jit_stat_steps2++;
		jit_step_end = true;
		return 2;
	}
	return 1;
}

/* its call in a block (k: instructions before it); the length, 0 if unknown */
static int jit_emit_step(const u8 *p, uword ip, unsigned k)
{
	int len = jit_insn_len(p);
	if (!len)
		return 0;
	jit_imm(1, ip | len << 16 | k << 24);
	jit_bl((uintptr_t)jit_step_tramp);
	jit_cck = JCC_NONE;
	return len;
}

static inline uint32_t jit_rd16(const u8 *p) { return p[0] | p[1] << 8; }
static inline uint32_t jit_rd32(const u8 *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

/* the instruction at page offset off (IP ip, k-th of the block): its code
 * (native or a helper's call); its length, or 0 to end the block before it */
static int jit_compile_insn(const u8 *base, uword off, uword ip, unsigned k, bool *ends)
{
	const u8 *p = base + off;
	int seg = 7, n = 0;
	bool o32 = false, a32 = false;
	u8 b;
	for (;;) {
		b = p[n++];
		if (b == 0x26) seg = SEG_ES;
		else if (b == 0x2e) seg = SEG_CS;
		else if (b == 0x36) seg = SEG_SS;
		else if (b == 0x3e) seg = SEG_DS;
		else if (b == 0x64) seg = SEG_FS;
		else if (b == 0x65) seg = SEG_GS;
		else if (b == 0x66) o32 = true;
		else if (b == 0x67) a32 = true;
		else break;
		if (n > 4) return 0;
	}
	int wv = o32 ? 2 : 1;                   /* the width of a v operand */
	uint32_t d = (uint32_t)seg << 8 | (a32 ? D_A32 : 0), x = 0;
	bool usex = false;
#define W(w) (d |= (uint32_t)(w) << 12)
#define MODRM() do { \
		u8 m = p[n++]; \
		d |= m; \
		int mod = m >> 6, rm = m & 7; \
		if (!a32) { \
			if (mod == 1) { d |= (uint32_t)(u16)(s8)p[n] << 16; n++; } \
			else if (mod == 2 || (mod == 0 && rm == 6)) { d |= jit_rd16(p + n) << 16; n += 2; } \
		} else if (mod != 3) { \
			if (rm == 4) { \
				u8 sib = p[n++]; \
				d |= (uint32_t)sib << 16; \
				if (mod == 0 && (sib & 7) == 5) { x = jit_rd32(p + n); n += 4; d |= D_XDISP; } \
			} \
			if (mod == 1) { x = (uint32_t)(int32_t)(s8)p[n]; n++; d |= D_XDISP; } \
			else if (mod == 2 || (mod == 0 && rm == 5)) { x = jit_rd32(p + n); n += 4; d |= D_XDISP; } \
		} \
		if (d & D_XDISP) usex = true; \
	} while (0)
	/* an immediate of width w (into x); a 67h displacement holds x already */
#define IMM(w) ({ \
		uint32_t _i = (w) == 0 ? p[n] : (w) == 1 ? jit_rd16(p + n) : jit_rd32(p + n); \
		n += 1 << (w); \
		_i; })
#define SETX(v) do { if (d & D_XDISP) return 0; x = (v); usex = true; } while (0)
#define NATIVE() do { *ends = false; return n; } while (0)
#define NATIVE_END() do { *ends = true; return n; } while (0)
	/* the end of a native memory form: its miss stub, the helper h(aux) */
#define MEM_END(h, aux) do { (void)(h); NATIVE(); } while (0)
	/* ALU Ev,imm on memory (op not ADC/SBB; 8 TEST) */
#define MEM_ALU_IMM(op, i) do { \
		int _w = D_W(d); \
		jit_nat_mem_begin(d, x, 1 << _w, (op) < 7, k, ip); \
		jit_ld12(0, _w, true); \
		jit_imm(1, (uint32_t)jit_sx(i, _w)); \
		int _r = jit_nat_alu_ops(op, _w); \
		if ((op) < 7) jit_st12(_r, _w); \
		MEM_END(jh_alu, (op) | 64); \
	} while (0)
#define CALL(h, aux, end) do { \
		*ends = (end); \
		if ((h) == jh_alu) jit_cck = jit_cck_of((aux) & 15); \
		else if ((h) == jh_incdec) jit_cck = JCC_INCDEC; \
		else if ((h) != jh_mov && (h) != jh_push && (h) != jh_pop && (h) != jh_lea && (h) != jh_xchg && \
			 (h) != jh_movx && (h) != jh_sreg) \
			jit_cck = JCC_NONE; \
		if (!jit_emit_call(h, d, (aux), usex, x, k, ip, end)) return 0; \
		return n; \
	} while (0)
	uword next;
	if (b < 0x40 && (b & 7) < 4) {                       /* ALU Eb,Gb Ev,Gv Gb,Eb Gv,Ev */
		int w = b & 1 ? wv : 0;
		W(w);
		MODRM();
		int op = (b >> 3) & 7;
		if (D_MOD(d) == 3 && jit_nat_alu(op, w, (b & 2) ? D_REG(d) : D_RM(d),
						 (b & 2) ? D_RM(d) : D_REG(d), false, 0))
			NATIVE();
		if (D_MOD(d) != 3 && ((op != 2 && op != 3) || jit_cf_known())) {
			bool tog = b & 2;
			jit_nat_mem_begin(d, x, 1 << w, !tog && op < 7, k, ip);
			if (tog) { jit_ldg(0, D_REG(d), w, true); jit_ld12(1, w, true); }
			else { jit_ld12(0, w, true); jit_ldg(1, D_REG(d), w, true); }
			int res = jit_nat_alu_ops(op, w);
			if (op < 7) { if (tog) jit_stg(res, D_REG(d), w); else jit_st12(res, w); }
			MEM_END(jh_alu, op | (tog ? 32 : 0));
		}
		CALL(jh_alu, op | ((b & 2) ? 32 : 0), false);
	}
	if (b < 0x40 && ((b & 7) == 4 || (b & 7) == 5)) {    /* ALU AL,Ib / eAX,Iv */
		int w = b & 1 ? wv : 0;
		W(w);
		d |= 0xc0 | ((b >> 3) & 7) << 3;                /* mod 3, rm 0, reg = the operation */
		uint32_t i = IMM(w);
		if (jit_nat_alu((b >> 3) & 7, w, 0, 0, true, i))
			NATIVE();
		SETX(i);
		CALL(jh_alu, ((b >> 3) & 7) | 64, false);
	}
	if (b >= 0x40 && b <= 0x4f) {                        /* INC/DEC r16|r32 */
		W(wv);
		if (jit_nat_incdec(b & 7, b & 8, wv, false, 0, 0, k, ip))
			NATIVE();
		CALL(jh_incdec, (b & 7) | (b & 8) | 32, false);
	}
	if (b >= 0x50 && b <= 0x57) {                        /* PUSH r */
		jit_nat_stack(true, 1 << wv, k, ip);
		jit_ldg(0, b & 7, wv, false);
		jit_st12(0, wv);
		jit_nat_sp(-(1 << wv));
		NATIVE();
	}
	if (b >= 0x58 && b <= 0x5f) {                        /* POP r (POP SP: the popped value) */
		jit_nat_stack(false, 1 << wv, k, ip);
		jit_ld12(0, wv, false);
		jit_nat_sp(1 << wv);
		jit_stg(0, b & 7, wv);
		NATIVE();
	}
	if (b == 0x68 || b == 0x6a) {                        /* PUSH imm */
		uint32_t i = b == 0x68 ? IMM(wv) : (uint32_t)(int32_t)(s8)p[n++];
		jit_nat_stack(true, 1 << wv, k, ip);
		jit_imm(0, i);
		jit_st12(0, wv);
		jit_nat_sp(-(1 << wv));
		NATIVE();
	}
	if ((b >= 0x70 && b <= 0x7f) || b == 0xeb || (b >= 0xe0 && b <= 0xe3)) {
		if (o32 || a32)
			return 0;
		s8 rel = p[n++];
		next = (ip + n) & 0xffff;
		uint32_t aux = next | (uint32_t)(u16)rel << 16;
		uword to = (next + rel) & 0xffff;
		d = b;
		if (b <= 0x7f) {
			int r = jit_nat_jcc(b & 15, k, next, to);
			if (r == 1)
				NATIVE();
			if (r == 2)
				NATIVE_END();
			CALL(jh_jcc, aux, true);
		}
		if (b == 0xeb) {
			jit_h(0x2000 | (k + 1));        /* movs r0, #k + 1 */
			jit_movw(1, to, false);
			NATIVE_END();
		}
		if (b >= 0xe2) {
			jit_nat_loop(b == 0xe3, k, next, to);
			NATIVE_END();
		}
		CALL(jh_loop, aux, true);
	}
	if (b >= 0xd8 && b <= 0xdf) {                        /* x87 */
		W(wv);
		MODRM();
		CALL(jh_esc, b - 0xd8, false);
	}
	if (b == 0x0f) {
		u8 b2 = p[n++];
		if (b2 >= 0x80 && b2 <= 0x8f) {                 /* Jcc rel16 */
			if (o32)
				return 0;
			u16 rel = jit_rd16(p + n);
			n += 2;
			next = (ip + n) & 0xffff;
			uword to = (next + rel) & 0xffff;
			d = 0x70 | (b2 & 15);
			int r = jit_nat_jcc(b2 & 15, k, next, to);
			if (r == 1)
				NATIVE();
			if (r == 2)
				NATIVE_END();
			CALL(jh_jcc, next | (uint32_t)rel << 16, true);
		}
		if (b2 == 0xa0 || b2 == 0xa8) { W(wv); CALL(jh_sreg, 2 | (b2 == 0xa0 ? SEG_FS : SEG_GS) << 8, false); }
		if (b2 == 0xa1 || b2 == 0xa9) { W(wv); CALL(jh_sreg, 3 | (b2 == 0xa1 ? SEG_FS : SEG_GS) << 8, false); }
		if (b2 == 0xb4 || b2 == 0xb5) {                 /* LFS, LGS */
			W(wv);
			MODRM();
			if (D_MOD(d) == 3) return 0;
			CALL(jh_sreg, 4 | (b2 == 0xb4 ? SEG_FS : SEG_GS) << 8, false);
		}
		if (b2 == 0xb6 || b2 == 0xb7 || b2 == 0xbe || b2 == 0xbf) {     /* MOVZX, MOVSX */
			W(wv);
			MODRM();
			int sw = b2 & 1;
			bool sx = b2 & 8;
			if (D_MOD(d) == 3) {
				jit_ldg(0, D_RM(d), sw, sx);
				jit_stg(0, D_REG(d), wv);
				NATIVE();
			}
			jit_nat_mem_begin(d, x, 1 << sw, false, k, ip);
			jit_ld12(0, sw, sx);
			jit_stg(0, D_REG(d), wv);
			MEM_END(jh_movx, sw | (sx ? 2 : 0));
		}
		return 0;
	}
	if (b >= 0x80 && b <= 0x83) {
		int w = b & 1 ? wv : 0;
		W(w);
		MODRM();
		uint32_t i = b == 0x81 ? IMM(wv) : b == 0x83 ? (uint32_t)(int32_t)(s8)p[n++] : p[n++];
		if (D_MOD(d) == 3 && jit_nat_alu(D_REG(d), w, D_RM(d), 0, true, i))
			NATIVE();
		int op = D_REG(d);
		if (D_MOD(d) != 3 && ((op != 2 && op != 3) || jit_cf_known()))
			MEM_ALU_IMM(op, i);
		SETX(i);
		CALL(jh_alu, D_REG(d) | 64, false);
	}
	if (b == 0x84 || b == 0x85) {
		int w = b & 1 ? wv : 0;
		W(w);
		MODRM();
		if (D_MOD(d) == 3 && jit_nat_alu(8, w, D_RM(d), D_REG(d), false, 0))
			NATIVE();
		jit_nat_mem_begin(d, x, 1 << w, false, k, ip);
		jit_ld12(0, w, true);
		jit_ldg(1, D_REG(d), w, true);
		jit_nat_alu_ops(8, w);
		MEM_END(jh_alu, 8);
	}
	if (b == 0xa8 || b == 0xa9) {
		int w = b & 1 ? wv : 0;
		W(w);
		d |= 0xc0;
		uint32_t i = IMM(w);
		if (jit_nat_alu(8, w, 0, 0, true, i))
			NATIVE();
		SETX(i);
		CALL(jh_alu, 8 | 64, false);
	}
	if (b == 0x86 || b == 0x87) { W(b & 1 ? wv : 0); MODRM(); CALL(jh_xchg, 0, false); }
	if (b >= 0x88 && b <= 0x8b) {
		int w = b & 1 ? wv : 0;
		W(w);
		MODRM();
		if (D_MOD(d) == 3) {
			if (b & 2) jit_nat_mov(D_REG(d), D_RM(d), w);
			else jit_nat_mov(D_RM(d), D_REG(d), w);
			NATIVE();
		}
		if (b & 2) {
			jit_nat_mem_begin(d, x, 1 << w, false, k, ip);
			jit_ld12(0, w, false);
			jit_stg(0, D_REG(d), w);
			MEM_END(jh_mov, 1);
		}
		jit_nat_mem_begin(d, x, 1 << w, true, k, ip);
		jit_ldg(0, D_REG(d), w, false);
		jit_st12(0, w);
		MEM_END(jh_mov, 0);
	}
	if (b >= 0xa0 && b <= 0xa3) {                        /* MOV AL|eAX, moffs / moffs, AL|eAX */
		int w = b & 1 ? wv : 0;
		W(w);
		if (a32) {
			d |= 0x05 | D_XDISP;            /* (mod 0, rm 5: disp32) */
			x = IMM(2);
			usex = true;
		} else
			d |= 0x06 | IMM(1) << 16;       /* (mod 0, rm 6: disp16) */
		jit_nat_mem_begin(d, x, 1 << w, b >= 0xa2, k, ip);
		if (b >= 0xa2) {
			jit_ldg(0, 0, w, false);
			jit_st12(0, w);
		} else {
			jit_ld12(0, w, false);
			jit_stg(0, 0, w);
		}
		NATIVE();
	}
	if (b == 0x8f) {                                     /* POP Ev */
		W(wv);
		MODRM();
		if (D_REG(d) != 0) return 0;
		CALL(jh_popm, 0, false);
	}
	if (b == 0xcd) {                                     /* INT n */
		uint32_t i = IMM(0);
		d = i;
		CALL(jh_int, (ip + n) & 0xffff, true);
	}
	if (b == 0xcf) {                                     /* IRET (16-bit) */
		if (o32) return 0;
		CALL(jh_iret, 0, true);
	}
	if (b == 0x8d) {
		W(wv);
		MODRM();
		if (D_MOD(d) == 3) return 0;
		CALL(jh_lea, 0, false);
	}
	if (b == 0x90)                                       /* NOP */
		NATIVE();
	if (b == 0x98) {                                     /* CBW, CWDE */
		jit_ldg(0, 0, wv - 1, true);
		jit_stg(0, 0, wv);
		NATIVE();
	}
	if (b == 0x99) {                                     /* CWD, CDQ */
		jit_ldg(0, 0, wv, true);
		jit_h(0x1000 | (wv == 2 ? 31 : 15) << 6 | 0 << 3 | 0);         /* asrs r0, r0, #15|31 */
		jit_stg(0, 2, wv);
		NATIVE();
	}
	if (b == 0x9b || b == 0xf5 || (b >= 0xf8 && b <= 0xfa) || b == 0xfc || b == 0xfd) {
		W(wv);
		CALL(jh_misc, b, false);
	}
	if (b >= 0x91 && b <= 0x97) { W(wv); CALL(jh_xchg, (b & 7) | 0x100, false); }
	if (b >= 0xb0 && b <= 0xb7) { jit_nat_mov_ri(b & 7, IMM(0), 0); NATIVE(); }
	if (b >= 0xb8 && b <= 0xbf) { jit_nat_mov_ri(b & 7, IMM(wv), wv); NATIVE(); }
	if (b == 0xc0 || b == 0xc1) {
		int w = b & 1 ? wv : 0;
		W(w);
		MODRM();
		u8 i = p[n++];
		if (jit_nat_shift(D_REG(d), i & 0x1f, D_RM(d), w, D_MOD(d) != 3, d, x, k, ip))
			NATIVE();
		CALL(jh_shift, 2 | (uint32_t)i << 8, false);
	}
	if (b >= 0xd0 && b <= 0xd3) {
		int w = b & 1 ? wv : 0;
		W(w);
		MODRM();
		if (b <= 0xd1 && jit_nat_shift(D_REG(d), 1, D_RM(d), w, D_MOD(d) != 3, d, x, k, ip))
			NATIVE();
		CALL(jh_shift, (b >> 1) & 1, false);
	}
	if (b == 0xc2 || b == 0xc3) {
		W(wv);
		uint32_t i = b == 0xc2 ? IMM(1) : 0;
		CALL(jh_ret, i, true);
	}
	if (b == 0xca || b == 0xcb) {                        /* RETF */
		W(wv);
		uint32_t i = b == 0xca ? IMM(1) : 0;
		CALL(jh_retf, i, true);
	}
	if (b == 0x9a) {                                     /* CALL ptr16:16|32 */
		W(wv);
		uint32_t o = IMM(wv), sel = IMM(1);
		next = (ip + n) & 0xffff;
		SETX(o);
		CALL(jh_callf, sel | (uint32_t)next << 16, true);
	}
	if (b == 0x8e || b == 0x8c) {                        /* MOV Sw,Ew / Ew,Sw */
		W(1);
		MODRM();
		int sg = D_REG(d);
		if (sg > SEG_GS || (b == 0x8e && (sg == SEG_CS || sg == SEG_SS)))
			return 0;
		if (b == 0x8e) {
			if (D_MOD(d) == 3)
				jit_ldg(0, D_RM(d), 1, false);
			else {
				jit_nat_mem_begin(d, x, 2, false, k, ip);
				jit_ld12(0, 1, false);
			}
			jit_nat_setseg(sg);
		} else {
			if (D_MOD(d) != 3)
				jit_nat_mem_begin(d, x, 2, true, k, ip);
			jit_ldr(0, JIT_SEGOFF(sg, sel));
			if (D_MOD(d) == 3)
				jit_stg(0, D_RM(d), 1);
			else
				jit_st12(0, 1);
		}
		NATIVE();
	}
	if (b == 0x06 || b == 0x0e || b == 0x16 || b == 0x1e) {     /* PUSH Sreg */
		jit_nat_stack(true, 1 << wv, k, ip);
		jit_ldr(0, JIT_SEGOFF(b >> 3, sel));
		jit_st12(0, wv);
		jit_nat_sp(-(1 << wv));
		NATIVE();
	}
	if (b == 0x07 || b == 0x1f) {                        /* POP ES, DS */
		jit_nat_stack(false, 1 << wv, k, ip);
		jit_ld12(0, wv, false);
		jit_nat_sp(1 << wv);
		jit_nat_setseg(b >> 3);
		NATIVE();
	}
	if (b == 0xc4 || b == 0xc5) {                        /* LES, LDS */
		W(wv);
		MODRM();
		if (D_MOD(d) == 3) return 0;
		if (!o32) {                             /* (one 4-byte read: offset, selector) */
			jit_nat_mem_begin(d, x, 4, false, k, ip);
			jit_h(0xF8BC); jit_h(0 << 12 | 2);      /* ldrh.w r0, [r12, #2] */
			jit_nat_setseg(b == 0xc4 ? SEG_ES : SEG_DS);
			jit_ld12(0, 1, false);
			jit_stg(0, D_REG(d), 1);
			NATIVE();
		}
		CALL(jh_sreg, 4 | (b == 0xc4 ? SEG_ES : SEG_DS) << 8, false);
	}
	if (b == 0xc6 || b == 0xc7) {
		int w = b & 1 ? wv : 0;
		W(w);
		MODRM();
		if (D_REG(d) != 0) return 0;
		uint32_t i = IMM(w);
		if (D_MOD(d) != 3) {
			jit_nat_mem_begin(d, x, 1 << w, true, k, ip);
			jit_imm(0, i);
			jit_st12(0, w);
			MEM_END(jh_mov, 2);
		}
		SETX(i);
		CALL(jh_mov, 2, false);
	}
	if (b == 0xe8 && o32) {                              /* CALL rel32 (66h) */
		W(2);
		uint32_t rel = IMM(2);
		next = (ip + n) & 0xffff;
		SETX(rel);
		CALL(jh_call, next, true);
	}
	if (b == 0xe8 || b == 0xe9) {
		if (o32)
			return 0;
		W(1);
		u16 rel = jit_rd16(p + n);
		n += 2;
		next = (ip + n) & 0xffff;
		if (b == 0xe9) {
			jit_h(0x2000 | (k + 1));
			jit_movw(1, (next + rel) & 0xffff, false);
			NATIVE_END();
		}
		CALL(jh_call, next | (uint32_t)rel << 16, true);
	}
	if (b == 0xf6 || b == 0xf7) {
		int w = b & 1 ? wv : 0;
		W(w);
		MODRM();
		int op = D_REG(d);
		if (op == 0 || op == 1) {
			uint32_t i = IMM(w);
			if (D_MOD(d) == 3 && jit_nat_alu(8, w, D_RM(d), 0, true, i))
				NATIVE();
			if (D_MOD(d) != 3)
				MEM_ALU_IMM(8, i);
			SETX(i);
			CALL(jh_alu, 8 | 64, false);
		}
		if (op >= 6) return 0;                  /* DIV, IDIV: #DE */
		CALL(jh_grp3, 0, false);
	}
	if (b == 0xfe || b == 0xff) {
		W(b & 1 ? wv : 0);
		MODRM();
		int op = D_REG(d);
		if (b == 0xff && op >= 2 && op <= 6) {
			if (op == 6)
				CALL(jh_ff, 0, false);
			CALL(jh_ff, (ip + n) & 0xffff, true);
		}
		if (op > 1) return 0;
		int w = b & 1 ? wv : 0;
		if (jit_nat_incdec(D_RM(d), op, w, D_MOD(d) != 3, d, x, k, ip))
			NATIVE();
		CALL(jh_incdec, op ? 8 : 0, false);
	}
	return 0;
#undef W
#undef MODRM
#undef IMM
#undef SETX
#undef NATIVE
#undef NATIVE_END
#undef MEM_END
#undef MEM_ALU_IMM
#undef CALL
}

/* compile the block at CS:IP (linear lin); its halfword offset + 1, or 0 */
static uint32_t jit_code_end;           /* (jit_compile: where the block's code ends) */

/* lin..end-1 has compiled code: stores to its pages go the slow way */
static __attribute__((noinline)) void jit_mark_code(CPUI386 *cpu, uword lin, uword end)
{
	for (uword c = lin >> JIT_CHUNK_SH; c <= (end - 1) >> JIT_CHUNK_SH; c++)
		jit_chunks[c >> 5] |= 1u << (c & 31);
	memset(jit_safe, 0, sizeof jit_safe);
	for (uword page = lin >> 12; page <= (end - 1) >> 12; page++) {
		jit_pages[page >> 5] |= 1u << (page & 31);
		struct tlb_entry *e = &cpu->tlb.tab[page % tlb_size];
		if (e->wtag == page)
			e->wtag = -1;
	}
	__asm volatile ("dsb\n\tisb" ::: "memory");
}

/* ---- frank-dos: the store ----
 * Every block compiled is also kept in PSRAM (main.c reserves it, jit16.h):
 * its key, the guest bytes it was compiled from, its count, its code and its
 * BLs. A block the SRAM cache dropped (evicted, or its table entry taken by
 * another key) is copied back from there, its BLs re-aimed, instead of being
 * compiled again: Doom 286's code is larger than the cache, and its blocks
 * were compiled ~900 times a second. The guest bytes are compared before a
 * block comes back, so code that changed since (self-modifying code,
 * overlays, another program) is compiled afresh. The store is a log: new
 * records overwrite the oldest; the index finds a record by its key and
 * checks its sequence number. */
#define JIT_SIDX_N      8192u           /* index entries, in sets of two */
struct jit_srec {
	uint32_t seq, key;
	uint16_t len;                   /* guest bytes */
	uint8_t count;                  /* JIT_FALLS | instructions */
	uint8_t nrel;
	uint16_t code_hw;
	uint16_t pad;
	/* then: guest bytes (len, to 4), rels (nrel x {u32 to, u32 off}), code */
};
struct jit_sidx { uint32_t key, seq_off; };     /* seq (15 bits) << 17 | word offset */
static struct jit_sidx *jit_sidx;
static uint8_t *jit_sdata;
static uint32_t jit_sbytes, jit_swpos, jit_sseq = 1;
uint32_t jit_stat_restores, jit_stat_stored, jit_stat_stalerec;

void jit_store_init(void *base, uint32_t bytes)
{
	jit_sidx = (struct jit_sidx *)base;
	memset(jit_sidx, 0, JIT_SIDX_N * sizeof *jit_sidx);
	jit_sdata = (uint8_t *)base + JIT_SIDX_N * sizeof *jit_sidx;
	jit_sbytes = bytes - JIT_SIDX_N * sizeof *jit_sidx;
	jit_swpos = 0;
}

static inline uint32_t jit_shash(uint32_t key)
{
	return ((key * 2654435761u) >> 19) & (JIT_SIDX_N / 2 - 1);
}

static __attribute__((noinline)) struct jit_srec *jit_store_find(uint32_t key)
{
	if (!jit_sidx)
		return NULL;
	struct jit_sidx *x = &jit_sidx[2 * jit_shash(key)];
	for (int w = 0; w < 2; w++, x++) {
		if (x->key != key || !x->seq_off)
			continue;
		uint32_t o = (x->seq_off & 0x1ffffu) * 4;
		if (o + sizeof(struct jit_srec) > jit_sbytes)
			continue;
		struct jit_srec *r = (struct jit_srec *)(jit_sdata + o);
		if (r->key == key && (r->seq & 0x7fffu) == x->seq_off >> 17)
			return r;
		jit_stat_stalerec++;
	}
	return NULL;
}

static __attribute__((noinline)) void jit_store_save(uint32_t key, const u8 *guest, uword len, uint8_t count, uint32_t start, uint32_t end)
{
	if (!jit_sidx || jit_rel_bad)
		return;
	uint32_t code_hw = end - start;
	uint32_t glen = (len + 3u) & ~3u;
	uint32_t size = sizeof(struct jit_srec) + glen + jit_nrel * 8u + ((code_hw * 2u + 3u) & ~3u);
	if (size > jit_sbytes / 4)
		return;
	if (jit_swpos + size > jit_sbytes)
		jit_swpos = 0;
	struct jit_srec *r = (struct jit_srec *)(jit_sdata + jit_swpos);
	uint8_t *p = (uint8_t *)(r + 1);
	r->seq = jit_sseq;
	r->key = key;
	r->len = len;
	r->count = count;
	r->nrel = (uint8_t)jit_nrel;
	r->code_hw = (uint16_t)code_hw;
	r->pad = 0;
	memcpy(p, guest, len);
	p += glen;
	for (unsigned i = 0; i < jit_nrel; i++, p += 8) {
		uint32_t rel[2] = { jit_rel_to[i], jit_rel_off[i] - start };
		memcpy(p, rel, 8);
	}
	memcpy(p, &jit_code[start], code_hw * 2u);
	struct jit_sidx *x = &jit_sidx[2 * jit_shash(key)];
	if (x->key != key)
		x[1] = x[0];                    /* (the newest first, the other one kept) */
	x->key = key;
	x->seq_off = (jit_sseq & 0x7fffu) << 17 | jit_swpos / 4;
	jit_sseq = (jit_sseq + 1) & 0x7fffu;
	if (!jit_sseq)
		jit_sseq = 1;
	jit_swpos += size;
	jit_stat_stored++;
}

/* a stored block back into the cache, if the guest's bytes are still the
 * ones it was compiled from: its offset + 1, 0 if none */
static __attribute__((noinline)) uint32_t jit_restore(CPUI386 *cpu, uint32_t key, uword lin, uint8_t *len, uint8_t *count)
{
	struct jit_srec *r = jit_store_find(key);
	if (!r || lin + r->len > JIT_LIN_END || !plain_page(cpu, lin) ||
	    (((lin + r->len - 1) ^ lin) & ~0xfffu && !plain_page(cpu, (lin + r->len - 1) & ~0xfffu)))
		return 0;
	const uint8_t *p = (const uint8_t *)(r + 1);
	if (memcmp(p, cpu->phys_mem + lin, r->len))
		return 0;
	p += (r->len + 3u) & ~3u;
	jit_make_room(r->code_hw + 2u);
	uint32_t start = jit_used;
	if (start + r->code_hw > jit_free_end)
		return 0;
	memcpy(&jit_code[start], p + r->nrel * 8u, r->code_hw * 2u);
	for (unsigned i = 0; i < r->nrel; i++) {
		uint32_t rel[2];
		memcpy(rel, p + i * 8u, 8);
		jit_pos = start + rel[1];
		jit_branch(jit_addr(jit_pos), rel[0], true);
	}
	jit_used = (start + r->code_hw + 1) & ~1u;
	*len = (uint8_t)r->len;
	*count = r->count;
	jit_mark_code(cpu, lin, lin + r->len);
	jit_stat_restores++;
	return start + 1;
}

static uint32_t __attribute__((noinline)) jit_compile(CPUI386 *cpu, uword lin, uword ip, uint8_t *len, bool *falls,
						   unsigned *ninsns)
{
	if (lin + 0x1000 > JIT_LIN_END)
		return 0;
	if (!plain_page(cpu, lin))
		return 0;
	const u8 *base = cpu->phys_mem + (lin & ~0xfffu);
	uword off = lin & 0xfff;
	/* into the next page too when it is plain memory (real mode's is
	 * contiguous): a block that stopped short of the page's end left the
	 * instructions there to the interpreter on every pass */
	uword lim = 0x1000;
	if (off > 0x1000 - JIT_MAX_BYTES && (lin & ~0xfffu) + 0x2000 <= JIT_LIN_END &&
	    plain_page(cpu, (lin & ~0xfffu) + 0x1000))
		lim = 0x2000;
	jit_make_room(64 + JIT_MAX_INSNS * 32);
	jit_pos = jit_used;
	jit_full = false;
	jit_nexits = 0;
	jit_nrel = 0;
	jit_rel_bad = false;
	uint32_t start = jit_pos;
	jit_h(0xB510);                          /* push {r4, lr} */
	jit_h(0x4604);                          /* mov r4, r0 */
	unsigned k = 0;
	bool ended = false;
	jit_cck = JCC_NONE;
	uword off0 = off;
	while (k < JIT_MAX_INSNS && off <= lim - 16 && ((ip + 15) & 0xffff) > ip && off - off0 <= JIT_MAX_BYTES - 15) {
		bool ends = false;
		/* (an instruction given up after some of its code: none of it stays) */
		uint32_t pos0 = jit_pos;
		int nexits0 = jit_nexits;
		unsigned nrel0 = jit_nrel;
		jit_nr3 = 0;
		int n = jit_compile_insn(base, off, ip, k, &ends);
		if (!n) {
			jit_pos = pos0;
			jit_nexits = nexits0;
			jit_nrel = nrel0;
			jit_nr3 = 0;
			n = jit_emit_step(base + off, ip, k);
			ends = false;
		}
		if (n && jit_nr3)
			jit_mem_resume(n, k);
		if (!n || jit_full)
			break;
		k++;
		off += n;
		ip = (ip + n) & 0xffff;
		if (ends) {
			ended = true;
			break;
		}
	}
	*falls = !ended;
	*ninsns = k;
	if (!k || jit_full) {
		jit_pos = start;
		return 0;
	}
	if (!ended) {
		/* the instruction after the last: where the interpreter goes on */
		jit_movw(1, ip, false);
		jit_h(0x2000 | k);
	}
	/* exit: str r1, [r4, #next_ip]; pop {r4, pc} */
	uint32_t exitpos = jit_pos;
	jit_h(0xF8C4);
	jit_h((1 << 12) | offsetof(CPUI386, next_ip));
	jit_h(0xBD10);
	if (jit_full) {
		jit_pos = start;
		return 0;
	}
	uint32_t save = jit_pos;
	for (int i = 0; i < jit_nexits; i++) {
		jit_pos = jit_exits[i];
		jit_branch(jit_addr(jit_exits[i]), jit_addr(exitpos), false);
	}
	jit_pos = save;
	jit_used = (jit_pos + 1) & ~1u;         /* (blocks start word-aligned) */
	uword end = (lin & ~0xfffu) + off;
	*len = end - lin;
	jit_mark_code(cpu, lin, end);
	jit_stat_blocks++;
	jit_code_end = jit_pos;
	return start + 1;
}

/* from cpu_exec1's loop head (real mode): the instructions compiled blocks ran
 * from CS:IP, one after another while each lands on the next and the slice
 * has budget left (IRQs wait for the slice's end in the interpreter too); 0
 * if none ran. cpu->next_ip is where to go on; *stop: it is an instruction a
 * block gave back or stopped before, for the interpreter, with a block
 * likely right after it. */
static int JIT_RAM(jit_enter)(CPUI386 *cpu, int budget, bool add, bool *stop)
{
	if (jit_stepping)
		return *stop = false, 0;
	if ((cpu->cr0 & 1) || !cpu->code16) {
		if (jit_used)
			jit_flush();
		return 0;
	}
	int total = 0;
	bool falls = false;
	*stop = false;
	do {
		uword ip = cpu->next_ip, csb = cpu->seg[SEG_CS].base;
		uword lin = csb + ip;
		if ((csb & 15) || csb > 0xffff0)
			break;
		uint32_t key = csb << 12 | ip;
		/* sets of two, the last entered first (two hot blocks on one
		 * entry threw each other out before JIT_THRESH) */
		uword h = (lin ^ (lin >> 8)) & (JIT_TAB_N / 2 - 1);
		struct jit_ent *e = &jit_tab[2 * h], *e2 = e + 1;
		if (e->key != key && e2->key == key) {
			struct jit_ent t = *e;
			*e = *e2;
			*e2 = t;
		} else if (e->key != key) {
			/* (the slice's first IP, add false, is anywhere in the code: an
			 * entry for each made straight-line loops' every instruction a
			 * key of its own, and their blocks' entries never got to
			 * JIT_THRESH, CheckIt's from VC -20%) */
			if (!add)
				break;
			*e2 = *e;
			jit_stat_rekeys++;
			e->key = key;
			e->count = 0;
			e->off = 0;
		}
		if (!e->off) {
			if (!jit_count_this() || (++e->count & ~JIT_DROPPED) < JIT_THRESH)
				break;
			jit_adapt();
			/* hot (again): from the store if it has it, rather than compiled */
			if (jit_sidx && !(e->count & JIT_DROPPED)) {
				uint8_t c;
				uint32_t o = jit_restore(cpu, key, lin, &e->len, &c);
				if (o) {
					e->off = o;
					e->count = c;
					goto run;
				}
			}
			bool f = false;
			unsigned ni = 0;
#if JIT_STATS
			if (e->count & JIT_EVICTED)
				jit_stat_recomp++;
#endif
			uint32_t o = jit_compile(cpu, lin, ip, &e->len, &f, &ni);
			e->off = o ? o : JIT_NO_CODE;
			if (!o)
				break;
			e->count = (e->count & JIT_DROPPED) | (f ? JIT_FALLS : 0) | ni;
			if (!(e->count & JIT_DROPPED))
				jit_store_save(key, cpu->phys_mem + lin, e->len, e->count, o - 1, jit_code_end);
		}
run:
		if (e->off == JIT_NO_CODE)
			break;
		/* (the slice's count exactly, as the interpreter's: the rest is its) */
		if ((int)(e->count & JIT_NINSNS) > budget - total)
			break;
		jit_block fn = (jit_block)(jit_addr(e->off - 1) | 1);
		uint32_t n = fn(cpu);
#if JIT_STATS
		jit_stat_runs++;
		jit_stat_insns += n;
#endif
		if (!n) {
			jit_stat_bails++;
			*stop = true;
			return total;
		}
		total += n;
		add = true;
		falls = e->count & JIT_FALLS;
		/* (a stepped HLT, STI, POPF, mode switch: back to the interpreter's loop) */
		if (unlikely(jit_step_end)) {
			jit_step_end = false;
			if (cpu->halt || cpu->stop || (cpu->intr && (cpu->flags & IF)) || (cpu->cr0 & 1) || !cpu->code16)
				break;
		}
	} while (total < budget);
	*stop = falls;
	return total;
}
