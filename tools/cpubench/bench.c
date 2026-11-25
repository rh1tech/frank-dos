/* cpubench: the i386 core on its own, for speed and for correctness.
 *
 *   bench IMAGE [ARG]     run a flat guest image (guest/NAME.c, linked at 0x400000)
 *                         to its HLT; print guest instructions, time, EAX
 *   fuzz SEED COUNT [V]   COUNT random instruction sequences from SEED; one
 *                         line per case: its number and a hash of the result
 *                         (V: the full state of each case instead)
 *
 * The same source builds for the host and for a Cortex-M33 under
 * qemu-system-arm -M mps2-an505 -icount shift=0 (tools/cpubench/run.sh),
 * where "time" is virtual time: 1 ns per executed M33 instruction.
 * Two cores (the reference, frozen from git, and the working one) give two
 * binaries; run.sh compares their fuzz output line by line. */
#include "i386.h"
#include "jit16.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define MEM_SIZE   (8u << 20)
#define PD_ADDR    0x7FF000u
#define PT_ADDR    0x7F0000u        /* 7 tables (bench) / 2 (fuzz) */
#define IMAGE_BASE 0x400000u
#define STACK_TOP  0x6F0000u

/* fuzz: every linear page maps somewhere (nothing faults by accident) */
#define POOL_ADDR  0x100000u
#define POOL_PAGES 4
#define CODE_LIN   0x00400000u      /* the sequence's page, read-only */
#define CODE_PHYS  0x200000u
#define FS_BASE    0x7FFDE000u
#define SEG_FS_IDX 4
/* FZMODE 3, real mode: the code at CS 2000h; ES DS SS FS GS on 30000h..4FFFFh */
#define RM_CODE    0x20000u
#define RM_DATA    0x30000u
#define RM_DATA_BYTES 0x20000u
#define SEG_GS_IDX 5

#ifdef CPUBENCH_ARM
extern uint64_t arm_time_ns(void);
extern void arm_init(void);
extern const unsigned char *arm_image(const char *name, unsigned *size);
#define now_ns() arm_time_ns()
static uint8_t *mem = (uint8_t *)0x80000000u;   /* mps2-an505 PSRAM, 16 MB */
#else
#include <time.h>
static uint64_t now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000u + ts.tv_nsec;
}
static uint8_t *mem;
#endif

/* RDTSC's source (the HLE's, on the board) */
uint64_t cpu_port_time_us(void) { return 0; }
#ifdef FX_KSTATS
unsigned fx_kstats[0x200];
#endif

static inline void w32(uint32_t pa, uint32_t v) { memcpy(mem + pa, &v, 4); }

static int g_exc = -1;
static uint32_t g_excerr;

static void exc_hook(CPUI386 *cpu, int excno, uword excerr)
{
	(void)cpu;
	g_exc = excno;
	g_excerr = excerr;
}

static u8 io_in8(void *o, int port) { (void)o; return port; }
static u16 io_in16(void *o, int port) { (void)o; return port * 3; }
static u32 io_in32(void *o, int port) { (void)o; return port * 5; }
static void io_out8(void *o, int port, u8 v) { (void)o; (void)port; (void)v; }
static void io_out16(void *o, int port, u16 v) { (void)o; (void)port; (void)v; }
static void io_out32(void *o, int port, u32 v) { (void)o; (void)port; (void)v; }

static CPUI386 *new_cpu(uint32_t eip)
{
	CPU_CB *cb;
	CPUI386 *cpu = cpui386_new(5, (char *)mem, MEM_SIZE, &cb);
	cb->io_read8 = io_in8; cb->io_read16 = io_in16; cb->io_read32 = io_in32;
	cb->io_write8 = io_out8; cb->io_write16 = io_out16; cb->io_write32 = io_out32;
	cpui386_enable_fpu(cpu);
	cpui386_reset_pm(cpu, eip);
	cpu->exc_hook = exc_hook;
	cpui386_set_paging(cpu, PD_ADDR);
	return cpu;
}

/* run to HLT or a fault; stepcount as the HLE gives it */
static void run(CPUI386 *cpu, uint64_t limit)
{
	g_exc = -1;
	uint64_t c0 = cpui386_get_cycle(cpu);
	while (!cpu->halt && g_exc < 0 && (uint64_t)(cpui386_get_cycle(cpu) - c0) < limit) {
		int sl = getenv("FZSLICE") ? atoi(getenv("FZSLICE")) : 1 << 14;
		uint64_t cb = cpui386_get_cycle(cpu);
		uint32_t ipb = cpu->next_ip;
		cpui386_step(cpu, sl);
		if (getenv("FZSLICE") && cpui386_get_cycle(cpu) - cb > (uint64_t)sl)
			printf("overrun: ip %x -> %x, %llu of %d\n", (unsigned)ipb, (unsigned)cpu->next_ip,
			       (unsigned long long)(cpui386_get_cycle(cpu) - cb), sl);
		if (getenv("FZCYC"))    /* (where each slice ended) */
			printf("slice: %llu ip %x\n", (unsigned long long)(cpui386_get_cycle(cpu) - c0), (unsigned)cpu->next_ip);
	}
}

/* ---- bench ------------------------------------------------------------- */

static int bench(const char *name, const uint8_t *img, unsigned size, uint32_t arg)
{
	memset(mem, 0, MEM_SIZE);
	for (uint32_t i = 0; i < 1024; i++)
		w32(PD_ADDR + i * 4, i < 7 ? (PT_ADDR + i * 4096) | 7 : 0);
	for (uint32_t p = 0; p < 7 * 1024; p++)
		w32(PT_ADDR + p * 4, (p << 12) | 7);
	memcpy(mem + IMAGE_BASE, img, size);
	CPUI386 *cpu = new_cpu(IMAGE_BASE);
	cpui386_set_gpr(cpu, 4, STACK_TOP);
	cpui386_set_gpr(cpu, 0, arg);
	cpu->seg[SEG_FS_IDX].base = 0;
	uint64_t t0 = now_ns();
	run(cpu, ~0ull);
	uint64_t t = now_ns() - t0;
	uint32_t eax = cpu->gprx[0].r32;
	long n = cpui386_get_cycle(cpu);
	printf("%-10s %10ld insns %10lu us  %6.1f ns/insn  eax=%08x%s\n", name, n,
	       (unsigned long)(t / 1000), n ? (double)t / n : 0.0, (unsigned)eax,
	       g_exc >= 0 ? "  FAULT" : "");
	if (g_exc >= 0)
		printf("  exception %d err %x at %08x\n", g_exc, (unsigned)g_excerr, (unsigned)cpu->ip);
	cpui386_delete(cpu);
	return g_exc >= 0;
}

/* ---- fuzz -------------------------------------------------------------- */

static uint64_t rs;
static uint32_t rnd(void)
{
	rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17;
	return (uint32_t)(rs >> 16);
}
static uint32_t rn(uint32_t n) { return rnd() % n; }

static uint8_t *cp;     /* the code being generated */
static void b(uint8_t v) { *cp++ = v; }
static void d32(uint32_t v) { b(v); b(v >> 8); b(v >> 16); b(v >> 24); }
static void d16(uint32_t v) { b(v); b(v >> 8); }

static uint32_t rval(void)
{
	switch (rn(11)) {
	case 8: case 9: return 0x00500000 + rn(8192);      /* pointers into a few pages */
	case 10: return 0x0012f000 + rn(4096);           /* a stack */
	case 0: return 0;
	case 1: return rn(16);
	case 2: return 0x80000000u + rn(4) - 2;
	case 3: return 0xFFFFFFFFu - rn(4);
	case 4: return 0x7FFFFFFFu - rn(4) + 2;
	case 5: return rn(256) << (rn(4) * 8);
	default: return rnd() ^ (rnd() << 16);
	}
}

/* ModRM (+SIB, disp) with reg field r; a16: 16-bit addressing */
static void modrm(int r, bool a16, bool regonly)
{
	int mod = regonly ? 3 : rn(4);
	int rm = rn(8);
	b(mod << 6 | r << 3 | rm);
	if (mod == 3)
		return;
	if (a16) {
		if (mod == 1) b(rnd());
		else if (mod == 2 || (mod == 0 && rm == 6)) d16(rnd());
		return;
	}
	if (rm == 4) {
		int base = rn(8);
		b(rn(4) << 6 | rn(8) << 3 | base);
		if (base == 5 && mod == 0) d32(rval());
	}
	if (mod == 1) b(rnd());
	else if (mod == 2 || (mod == 0 && rm == 5)) d32(rval());
}

static void imm(int size) { if (size == 1) b(rnd()); else if (size == 2) d16(rnd()); else d32(rval()); }

static int kmode;        /* fuzzk: what fastloop_m33.S takes (no prefixes, strings, misc) */
static int rm16;         /* FZMODE 3: real mode, 16-bit code (66h/67h switch to 32 bits);
                         * 4: the same in 16-bit protected mode (CR0.PE, 64 KB segments) */
static char jsz[64];     /* each branch field's size: 1, 2 or 4 bytes */
static int jmode;        /* FZJIT (with FZMODE 3): only what the JIT takes */
static uint8_t *jfix[64];
static char jabs[64];        /* an absolute target (CODE_LIN based), not rel */
static int jfix_insn[64];   /* the instruction each belongs to */
static int njfix, ninsn;

/* one random instruction */
static void gen_insn(void)
{
	bool o16 = rn(6) == 0, a16 = rn(16) == 0;
	int seg = rn(10);
	int k = rn(100);
	if (rm16 && jmode) {    /* FZJIT: what jit16.inc compiles (1: no 66h/67h or 0F; 2: with them) */
		if (jmode == 1)
			o16 = a16 = false;
		else
			o16 = rn(3) == 0, a16 = rn(4) == 0;
		static const uint8_t ks[] = { 0, 5, 10, 15, 21, 22, 24, 26, 30, 34, 36, 38, 40, 42, 44, 46, 47, 48, 49,
					      50, 52, 54, 56, 60, 63, 64, 74, 76, 78, 79, 94, 95 };
		k = ks[rn(sizeof ks)];
	}
	if (kmode) {
		o16 = a16 = false;
		seg = 9;
		if (k >= 80 && k < 97)
			k = rn(80);
		if (rn(4) == 0) {           /* a 32-bit CMP, then a Jcc rel8 (fastloop_m33.S fuses them) */
			switch (rn(5)) {
			case 0: b(0x39); modrm(rn(8), false, rn(2) == 0); break;
			case 1: b(0x3B); modrm(rn(8), false, rn(2) == 0); break;
			case 2: b(0x83); modrm(7, false, rn(2) == 0); b(rnd()); break;
			case 3: b(0x81); modrm(7, false, rn(2) == 0); d32(rval()); break;
			case 4: b(0x3D); d32(rval()); break;
			}
			b(0x70 + rn(16));
			jfix_insn[njfix] = ninsn, jabs[njfix] = 0, jsz[njfix] = 1, jfix[njfix++] = cp;
			b(0);
			return;
		}
		if (rn(5) == 0) {           /* indirect transfers to a later instruction: CALL/JMP reg, RET, CALL/JMP [mem] */
			int r = rn(8);
			if (r == 4) r = 0;
			uint32_t slot = 0x00500000 + (rn(1024) & ~3u);
			switch (rn(6)) {
			case 0: case 1:
				b(0xB8 + r); jfix_insn[njfix] = ninsn, jabs[njfix] = 1, jsz[njfix] = 4, jfix[njfix++] = cp; d32(0);
				b(0xFF); b((rn(2) ? 0xD0 : 0xE0) + r);
				break;
			case 2: case 3:
				b(0x68); jfix_insn[njfix] = ninsn, jabs[njfix] = 1, jsz[njfix] = 4, jfix[njfix++] = cp; d32(0);
				if (rn(2)) b(0xC3); else { b(0xC2); d16(rn(3) * 4); }
				break;
			default:
				b(0xC7); b(0x05); d32(slot); jfix_insn[njfix] = ninsn, jabs[njfix] = 1, jsz[njfix] = 4, jfix[njfix++] = cp; d32(0);
				b(0xFF); b(rn(2) ? 0x15 : 0x25); d32(slot);
				break;
			}
			return;
		}
	}
	if (rm16 && jmode != 1 && rn(14) == 0) {    /* real mode: segment loads and stores (not CS, SS) */
		static const uint8_t sr[] = { 0, 3, 4, 5 };
		switch (rn(6)) {
		case 0: b(0x8E); modrm(sr[rn(4)], a16 ^ rm16, rn(2) == 0); break;
		case 1: b(0x8C); modrm(rn(6), a16 ^ rm16, rn(2) == 0); break;
		case 2: b("\x06\x0e\x16\x1e"[rn(4)]); break;
		case 3: b(rn(2) ? 0x07 : 0x1F); break;
		case 4: { uint8_t *m; b(0xC4 + rn(2)); m = cp; do { cp = m; modrm(rn(8), a16 ^ rm16, false); } while ((*m >> 6) == 3); } break;
		case 5: b(0x0F); b("\xa0\xa1\xa8\xa9"[rn(4)]); break;
		}
		return;
	}
	int sub0f = rn(11);
	if (jmode == 2 && k >= 64 && k < 74)
		sub0f = 0;              /* (of 0F: Jcc near; MOVZX/MOVSX come with the MOV forms) */
	if ((k >= 74 && k < 80) || (k >= 64 && k < 74 && sub0f == 0))
		o16 = a16 = false;          /* (a 16-bit IP leaves the code page) */
	if (k >= 80 && k < 88) {            /* string ops: ECX bounded, DF either way, first */
		if (rm16) b(0x66);      /* (all of ECX: a 67h REP counts with it) */
		b(0xB9); d32(rn(4) ? rn(40) : rn(5000));
		b(rn(4) ? 0xFC : 0xFD);
	}
	if (seg < 6 && rn(3) == 0) b("\x26\x2e\x36\x3e\x64\x65"[seg]);
	if (o16) b(0x66);
	if (a16) b(0x67);
	o16 ^= rm16;            /* from here on: the operand / address size is 16 bits */
	a16 ^= rm16;
	int vsz = o16 ? 2 : 4;
	if (k < 22) {                       /* ALU r/m, r and r, r/m */
		b((rn(8) << 3) | rn(4));
		modrm(rn(8), a16, rn(3) == 0);
	} else if (k < 26) {                /* ALU acc, imm */
		int op = rn(8), w = rn(2);
		b(op << 3 | 4 | w);
		imm(w ? vsz : 1);
	} else if (k < 34) {                /* 80/81/83 */
		int o = (uint8_t)"\x80\x81\x83"[rn(3)];
		b(o);
		modrm(rn(8), a16, rn(3) == 0);
		imm(o == 0x81 ? vsz : 1);
	} else if (k < 38) {                /* INC/DEC/PUSH/POP reg */
		int o = 0x40 + rn(32);
		if (o >= 0x50 && rn(2)) o &= ~8;        /* more pushes than pops */
		b(o);
	} else if (k < 46) {                /* MOV forms */
		switch (rn(6)) {
		case 0: b(0x88 + rn(4)); modrm(rn(8), a16, rn(3) == 0); break;
		case 1: { int w = rn(2); b(0xC6 + w); modrm(0, a16, false); imm(w ? vsz : 1); } break;
		case 2: { int o = 0xB0 + rn(16); b(o); imm(o >= 0xB8 ? vsz : 1); } break;
		case 3: b(0xA0 + rn(4)); if (a16) d16(rnd()); else d32(rval()); break;
		case 4: b(0x8D); { uint8_t *m = cp; do { cp = m; modrm(rn(8), a16, false); } while ((*m >> 6) == 3); } break;           /* LEA */
		case 5: b(0x0F); b("\xB6\xB7\xBE\xBF"[rn(4)]); modrm(rn(8), a16, rn(3) == 0); break;
		}
	} else if (k < 50) {                /* XCHG, TEST */
		switch (rn(4)) {
		case 0: b(0x86 + rn(2)); modrm(rn(8), a16, rn(2) == 0); break;
		case 1: b(0x90 + rn(8)); break;
		case 2: b(0x84 + rn(2)); modrm(rn(8), a16, rn(3) == 0); break;
		case 3: { int w = rn(2); b(0xA8 + w); imm(w ? vsz : 1); } break;
		}
	} else if (k < 56) {                /* F6/F7 group */
		int w = rn(2), r = rn(8);
		if (r >= 6 && rn(3)) r = rn(6);  /* fewer divides (most fault) */
		b(0xF6 + w);
		modrm(r, a16, rn(2) == 0);
		if (r == 0 || r == 1) imm(w ? vsz : 1);
	} else if (k < 64) {                /* shifts and rotates */
		int o = (uint8_t)"\xC0\xC1\xD0\xD1\xD2\xD3"[rn(6)];
		b(o);
		modrm(rn(8), a16, rn(2) == 0);
		if (o <= 0xC1) b(rn(4) ? rn(34) : rnd());
	} else if (k < 74) {                /* 0F: Jcc near, SETcc, CMOV, IMUL, BT.., BSF/BSR, SHxD, BSWAP, XADD, CMPXCHG */
		b(0x0F);
		switch (sub0f) {
		case 0: b(0x80 + rn(16)); jfix_insn[njfix] = ninsn, jabs[njfix] = 0, jsz[njfix] = o16 ? 2 : 4, jfix[njfix++] = cp;
			if (o16) d16(0); else d32(0);
			break;
		case 1: b(0x90 + rn(16)); modrm(0, a16, rn(2) == 0); break;
		case 2: b(0x40 + rn(16)); modrm(rn(8), a16, rn(2) == 0); break;
		case 3: b(0xAF); modrm(rn(8), a16, rn(2) == 0); break;
		case 4: b("\xA3\xAB\xB3\xBB"[rn(4)]); modrm(rn(8), a16, rn(2) != 0); break;
		case 5: b(0xBA); modrm(4 + rn(4), a16, rn(2) == 0); b(rnd()); break;
		case 6: b(0xBC + rn(2)); modrm(rn(8), a16, rn(2) == 0); break;
		case 7: { int o = (uint8_t)"\xA4\xA5\xAC\xAD"[rn(4)]; b(o); modrm(rn(8), a16, rn(2) == 0); if (!(o & 1)) b(rn(40)); } break;
		case 8: b(0xC8 + rn(8)); break;
		case 9: b(0xC0 + rn(2)); modrm(rn(8), a16, rn(2) == 0); break;
		case 10: b(0xB0 + rn(2)); modrm(rn(8), a16, rn(2) == 0); break;
		}
	} else if (k < 80) {                /* short branches (forward, fixed up later) */
		switch (rn(5)) {
		case 0: case 1: b(0x70 + rn(16)); jfix_insn[njfix] = ninsn, jabs[njfix] = 0, jsz[njfix] = 1, jfix[njfix++] = cp; b(0); break;
		case 2: b(0xEB); jfix_insn[njfix] = ninsn, jabs[njfix] = 0, jsz[njfix] = 1, jfix[njfix++] = cp; b(0); break;
		case 3: b(0xE2 + rn(2)); jfix_insn[njfix] = ninsn, jabs[njfix] = 0, jsz[njfix] = 1, jfix[njfix++] = cp; b(0); break;        /* LOOP, JECXZ */
		case 4: b(0xE8); jfix_insn[njfix] = ninsn, jabs[njfix] = 0, jsz[njfix] = o16 ? 2 : 4, jfix[njfix++] = cp;       /* CALL */
			if (o16) d16(0); else d32(0);
			break;
		}
	} else if (k < 88) {                /* string ops */
		int o = 0xA4 + rn(12);
		if (o == 0xA8 || o == 0xA9) o = 0xA4;
		int rep = rn(3);
		if (rep == 1) b(0xF3);
		if (rep == 2) b(0xF2);
		b(o);
	} else if (k < 94) {                /* one-byte misc */
		static const uint8_t ops[] = {0x98, 0x99, 0x9E, 0x9F, 0x9C, 0xF5, 0xF8, 0xF9,
			0x27, 0x2F, 0x37, 0x3F, 0xD7, 0xFC, 0xFD, 0x9D, 0xC9, 0x60, 0x61};
		b(ops[rn(sizeof ops)]);
	} else if (k < 97) {                /* PUSH imm, IMUL imm, ENTER */
		switch (rn(4)) {
		case 0: b(0x68); imm(vsz); break;
		case 1: b(0x6A); b(rnd()); break;
		case 2: b(0x69); modrm(rn(8), a16, rn(2) == 0); imm(vsz); break;
		case 3: b(0x6B); modrm(rn(8), a16, rn(2) == 0); b(rnd()); break;
		}
	} else {                            /* FE/FF inc/dec/push, AAM/AAD */
		switch (rn(4)) {
		case 3: b(0xD8 + rn(8)); modrm(rn(8), a16, rn(2) == 0); break;      /* x87 */
		case 0: b(0xFE); modrm(rn(2), a16, rn(2) == 0); break;
		case 1: b(0xFF); modrm("\x00\x01\x06"[rn(3)], a16, rn(2) == 0); break;
		case 2: b(0xD4 + rn(2)); b(rn(8) ? 10 + rn(10) : rn(3)); break;
		}
	}
}

static uint32_t fnv(uint32_t h, const void *p, unsigned n)
{
	const uint8_t *s = p;
	for (unsigned i = 0; i < n; i++)
		h = (h ^ s[i]) * 16777619u;
	return h;
}

static void fuzz_setup(void)
{
	memset(mem, 0, MEM_SIZE);
	/* PT 0: every page to the pool; PT 1 (0x400000..): the code page first */
	for (uint32_t i = 0; i < 1024; i++) {
		w32(PT_ADDR + i * 4, (POOL_ADDR + (i % POOL_PAGES) * 4096) | 7);
		w32(PT_ADDR + 4096 + i * 4, i < 2 ? (CODE_PHYS + i * 4096) | 5 : (POOL_ADDR + (i % POOL_PAGES) * 4096) | 7);
	}
	for (uint32_t i = 0; i < 1024; i++)
		w32(PD_ADDR + i * 4, (i == (CODE_LIN >> 22) ? PT_ADDR + 4096 : PT_ADDR) | 7);
}

/* ---- bench16: a 16-bit real-mode image at 2000:0000, to its HLT --------- */

#ifdef FX16_FBSTATS
unsigned fx16_fb[0x200];
#endif
static int bench16(const char *name, const uint8_t *img, unsigned size)
{
	memset(mem, 0, 0x100000);
	memcpy(mem + 0x20000, img, size);
	jit_note_write(0, 0x100000);
	CPUI386 *cpu = new_cpu(0);
	cpui386_reset(cpu);
	cpui386_flush_tlb(cpu);
	cpu->seg[1].sel = 0x2000;
	cpu->seg[1].base = 0x20000;
	cpu->next_ip = 0;
	uint64_t t0 = now_ns();
	run(cpu, ~0ull);
	uint64_t t = now_ns() - t0;
	long n = cpui386_get_cycle(cpu);
	uint32_t h = fnv(2166136261u, cpu->gprx, 32);
	h = fnv(h, mem + 0x30000, 0x10000);
	printf("%-10s %10ld insns %10lu us  %6.1f ns/insn  state=%08x%s\n", name, n, (unsigned long)(t / 1000),
	       n ? (double)t / n : 0.0, (unsigned)h, g_exc >= 0 ? "  FAULT" : "");
#ifdef FX16_FBSTATS
	{       /* (the fast loop's fall backs, the top ones) */
		extern unsigned fx16_fb[0x200];
		for (int k = 0; k < 12; k++) {
			int m = 0;
			for (int i = 1; i < 0x200; i++) if (fx16_fb[i] > fx16_fb[m]) m = i;
			if (!fx16_fb[m]) break;
			printf("  %s%02x:%u", m & 0x100 ? "66/67 " : "", m & 0xff, fx16_fb[m]);
			fx16_fb[m] = 0;
		}
		printf("\n");
		memset(fx16_fb, 0, sizeof fx16_fb);
	}
#endif
	cpui386_delete(cpu);
	return g_exc >= 0;
}

static int fzmode;
static long fz_limit;
static int fuzz(uint64_t seed, unsigned start, unsigned count, int verbose)
{
#ifdef FZMODE_FIXED
	fzmode = FZMODE_FIXED;
#else
	fzmode = getenv("FZMODE") ? atoi(getenv("FZMODE")) : 0;
#endif
	fuzz_setup();
	CPUI386 *cpu = new_cpu(CODE_LIN);
	if (fzmode == 1) {
		cpu->cr0 &= ~(1u << 31);
		cpui386_flush_tlb(cpu);
	}
	rm16 = fzmode == 3 || fzmode == 4;
#ifdef FZJIT_FIXED
	jmode = FZJIT_FIXED;
#else
	jmode = getenv("FZJIT") ? atoi(getenv("FZJIT")) : 0;
#endif
	if (rm16) {             /* real mode: CS 2000h, data segments in 30000h..4FFFFh */
		cpui386_reset(cpu);
		if (fzmode == 4)
			cpu->cr0 |= 1;  /* (16-bit protected mode: no paging, CPL 0) */
		cpui386_flush_tlb(cpu);
	}
	uint32_t code_phys = fzmode == 1 ? CODE_LIN : rm16 ? RM_CODE : CODE_PHYS;
	unsigned long ctx = cpui386_ctx_size();
	void *ctx0 = malloc(ctx);
	cpui386_ctx_save(cpu, ctx0);
	unsigned diffs = 0;
	(void)diffs;
	for (unsigned c = start; c < start + count; c++) {
		rs = seed * 0x9E3779B97F4A7C15ull + c * 0xD1B54A32D192ED03ull + 1;
		for (int i = 0; i < 16; i++) rnd();
		/* the code pages: a sequence, then HLT to the end; most start
		 * near the end of the first, so instructions straddle the two */
		uint8_t *code = mem + code_phys;
		memset(code, 0xF4, 8192);
		uint32_t org = rn(2) ? 4096 - 1 - rn(120) : rn(2048);
		code += org;
		cp = code;
		njfix = 0;
		int n = 4 + rn(40);
		uint8_t *ends[64];
		int ng = 0;
		while (ng < n && cp < code + 3800 && njfix < 60) {
			ninsn = ng;
			gen_insn();
			ends[ng++] = cp;
		}
		n = ng;
		uint8_t *end = cp;
		for (int i = 0; i < njfix; i++) {
			/* forward, to an instruction boundary (never between a string
			 * op and its count) or into the HLTs */
			uint8_t *f = jfix[i];
			if (jabs[i]) {
				int j = jfix_insn[i] + 1 + rn(4);
				uint8_t *to = j < n ? ends[j] : end + rn(4);
				uint32_t lin = CODE_LIN + (uint32_t)(to - (mem + code_phys));
				memcpy(f, &lin, 4);
				continue;
			}
			uint8_t *from = f + jsz[i];
			int j = jfix_insn[i] + 1 + rn(4);
			uint8_t *to = j < n ? ends[j] : end + rn(4);
			if (jsz[i] == 1 && to - from > 127) to = ends[jfix_insn[i]];
			int32_t d = (int32_t)(to - from);
			if (jsz[i] == 1) *f = (uint8_t)d; else memcpy(f, &d, jsz[i]);
		}
		/* the pool: random bytes */
		if (rm16)
			for (uint32_t i = 0; i < RM_DATA_BYTES; i += 4)
				w32(RM_DATA + i, rn(4) ? rnd() ^ (rnd() << 16) : rval());
		else
			for (uint32_t i = 0; i < POOL_PAGES * 4096; i += 4)
				w32(POOL_ADDR + i, rn(4) ? rnd() ^ (rnd() << 16) : rval());
		/* (the new code was written behind the CPU's back, as a DMA would) */
		jit_note_write(code_phys, 8192);
		cpui386_ctx_restore(cpu, ctx0);
		/* (the page tables never change: a TLB kept warm across cases lets
		 * the fast paths, which need its tags, run most accesses) */
		if (c % 64 == 0)
			cpui386_flush_tlb(cpu);
		for (int i = 0; i < 8; i++)
			cpui386_set_gpr(cpu, i, rn(5) < 3 ? 0x00500000 + rn(8192) : rval());
		if (rn(3))
			cpui386_set_gpr(cpu, 4, 0x0012f800 + (rn(256) & ~3u));     /* ESP: a stack, mostly */
		cpu->next_ip = CODE_LIN + org;
		cpu->halt = false;
		cpu->flags = 0x2 | (rnd() & (0x8D5)) ;
		cpu->cc.mask = 0;
		cpu->seg[SEG_FS_IDX].sel = 0x3B;
		cpu->seg[SEG_FS_IDX].base = FS_BASE;
		cpu->seg[SEG_FS_IDX].limit = 0xFFF;
		cpu->seg[SEG_FS_IDX].flags = 0x40F3 >> 0;
		cpu->seg[SEG_GS_IDX] = cpu->seg[SEG_FS_IDX];
		cpu->seg[SEG_GS_IDX].base = 0x12340000;
		if (rm16) {
			static const uint16_t sel[6] = { 0x3000, RM_CODE >> 4, 0x3800, 0x3400, 0x3c00, 0x4000 };
			for (int sg = 0; sg < 6; sg++) {
				cpu->seg[sg].sel = sel[sg];
				cpu->seg[sg].base = (uint32_t)sel[sg] << 4;
				cpu->seg[sg].limit = 0xffff;
				cpu->seg[sg].flags = fzmode == 4 ? (sg == 1 ? 0x9b : 0x93) : 0;
			}
			for (int i = 0; i < 8; i++)
				cpui386_set_gpr(cpu, i, rn(5) < 3 ? rn(0x10000) : rval());
			if (rn(3))
				cpui386_set_gpr(cpu, 4, 0x8000 + (rn(256) & ~1u));
			cpu->next_ip = org;
			cpu->sp_mask = 0xffff;
			if (fzmode == 4 && rn(2)) {     /* (a 32-bit stack, as DOS/4GW's 16-bit code has) */
				cpu->seg[2].flags |= 1 << 14;   /* (SS.B) */
				cpu->sp_mask = 0xffffffff;
				cpui386_set_gpr(cpu, 4, 0x8000 + (rn(256) & ~1u));
			}
		}
		if (fzmode == 2)
			for (int sg = 0; sg < 4; sg++)
				if (sg != 1)    /* ES SS DS (CS stays flat: the code's linear address) */
					cpu->seg[sg].base = 0x10000;
		if (verbose > 1) {
			for (uint8_t *p = code, k = 0; p < end; p++) {
				printf("%02x", *p);
				if (k < n && p + 1 == ends[k]) printf(" "), k++;
			}
			printf("\n");
			fflush(stdout);
		}
#ifdef FZLIMIT_FIXED
		fz_limit = FZLIMIT_FIXED;
#else
		if (getenv("FZLIMIT")) fz_limit = atol(getenv("FZLIMIT"));
		if (getenv("FZLIMITC") && c != (unsigned)atol(getenv("FZLIMITC")))
			fz_limit = 0;   /* (the limit on that case only, after the others) */
#endif
		if (fz_limit) {         /* (bisecting a case: exactly fz_limit instructions) */
			g_exc = -1;
			long c0 = cpui386_get_cycle(cpu);
			while (!cpu->halt && g_exc < 0 && cpui386_get_cycle(cpu) - c0 < fz_limit)
				cpui386_step(cpu, 1);
		} else
			run(cpu, 20000);
		uint32_t st[16];
		for (int i = 0; i < 8; i++) st[i] = cpu->gprx[i].r32;
		st[8] = cpu_getflags(cpu);
		st[9] = g_exc >= 0 ? cpu->ip : cpu->next_ip;
		st[10] = g_exc;
		st[11] = g_exc >= 0 ? g_excerr : 0;
		st[12] = g_exc == 14 ? cpu->cr2 : 0;
		st[13] = cpu->halt;
		st[14] = 0;
		st[15] = 0;
		uint32_t h = fnv(2166136261u, st, sizeof st);
		h = fnv(h, mem + POOL_ADDR, POOL_PAGES * 4096);
		if (rm16) {
			h = fnv(h, mem + RM_DATA, RM_DATA_BYTES);
			h = fnv(h, mem + RM_CODE, 8192);        /* (self-modifying stores) */
		}
		if (fzmode == 1) {      /* (no pool aliasing: the regions the registers point at) */
			h = fnv(h, mem + 0x500000, 0x6000);
			h = fnv(h, mem + 0x12f000, 0x2000);
			h = fnv(h, mem + 0x510000, 0x6000);
		}
		if (verbose) {
			printf("%u:", c);
			for (int i = 0; i < 14; i++) printf(" %08x", (unsigned)st[i]);
			printf(" mem=%08x", fnv(2166136261u, mem + POOL_ADDR, POOL_PAGES * 4096));
			if (rm16)       /* (frank-dos: the real-mode data and code, in the hash too) */
				printf(" rmdata=%08x rmcode=%08x", fnv(2166136261u, mem + RM_DATA, RM_DATA_BYTES),
				       fnv(2166136261u, mem + RM_CODE, 8192));
			if (rm16 && getenv("FZDUMP")) {         /* (the real-mode data, for cmp) */
				FILE *df = fopen(getenv("FZDUMP"), "wb");
				if (df) fwrite(mem, 1, MEM_SIZE + 65536, df), fclose(df);
			}
			printf(" code=");
			for (uint8_t *p = code; p < end; p++) printf("%02x", *p);
			printf("\n");
			fflush(stdout);
		} else
			printf("%u %08x\n", c, (unsigned)h);
	}
	free(ctx0);
	cpui386_delete(cpu);
	return 0;
}

/* ---- replay (M33): a guest state from the host (hle.c's FRANK_SNAP) run on
 * with the kernel and without it, to the next native call, compared ------ */
#ifdef ARM_REPLAY
int fx_kernel_on = 1;
extern int arm_fopen(const char *path);
extern unsigned arm_fread(int h, void *buf, unsigned n);
extern void arm_fclose(int h);
static int r_stop;
static bool r_trap(CPUI386 *cpu) { (void)cpu; r_stop = 1; return false; }

static CPUI386 *r_cpu(uint8_t *m, uint32_t size, const uint32_t *h, const void *fpu)
{
	CPUI386 *cpu = cpui386_new(5, (char *)m, size, NULL);
	cpui386_enable_fpu(cpu);
	cpui386_reset_pm(cpu, h[10]);
	for (int i = 0; i < 8; i++) cpu->gprx[i].r32 = h[2 + i];
	cpu->flags = h[11]; cpu->cc.mask = 0;
	cpu->cpl = h[12]; cpu->code16 = h[13]; cpu->sp_mask = h[14];
	cpu->cr0 = h[15]; cpu->cr2 = h[16];
	cpu->idt.base = h[18]; cpu->idt.limit = h[19]; cpu->gdt.base = h[20]; cpu->gdt.limit = h[21];
	for (int i = 0; i < 8; i++) {
		cpu->seg[i].sel = h[22 + i * 4]; cpu->seg[i].base = h[23 + i * 4];
		cpu->seg[i].limit = h[24 + i * 4]; cpu->seg[i].flags = h[25 + i * 4];
	}
	memcpy(cpu->fpu, fpu, h[54]);
	cpui386_set_paging(cpu, h[17]);
	cpu->cr0 = h[15];
	cpu->hle_trap = r_trap;
	cpu->exc_hook = exc_hook;
	return cpu;
}

static void r_run(CPUI386 *cpu)
{
	r_stop = 0;
	g_exc = -1;
	long c0 = cpui386_get_cycle(cpu);
	while (!r_stop && !cpu->halt && g_exc < 0 && cpui386_get_cycle(cpu) - c0 < 3000000)
		cpui386_step(cpu, 1 << 14);
}

static void replay(const char *dir)
{
	uint8_t *ma = (uint8_t *)0x80000000u, *mb = (uint8_t *)0x80800000u;
	static uint32_t fpubuf[64];
	for (int k = 0;; k++) {
		char path[300];
		uint32_t h[64];
		snprintf(path, sizeof path, "%s/s%05d.bin", dir, k);
		int fd = arm_fopen(path);
		if (fd < 0)
			break;
		arm_fread(fd, h, 256);
		arm_fread(fd, fpubuf, h[54]);
		uint32_t size = h[55];
		arm_fread(fd, ma, size);
		arm_fclose(fd);
		memcpy(mb, ma, size);
		memset(ma + size, 0, 65536);
		memset(mb + size, 0, 65536);
		uint32_t st[2][14];
		int stop[2], exc[2];
		long n[2];
		for (int v = 0; v < 2; v++) {
			fx_kernel_on = v == 0;
			CPUI386 *cpu = r_cpu(v ? mb : ma, size, h, fpubuf);
			r_run(cpu);
			for (int i = 0; i < 8; i++) st[v][i] = cpu->gprx[i].r32;
			st[v][8] = cpu_getflags(cpu);
			st[v][9] = cpu->ip; st[v][10] = cpu->next_ip;
			st[v][11] = cpu->cr2; st[v][12] = g_exc; st[v][13] = cpu->halt;
			stop[v] = r_stop; exc[v] = g_exc;
			n[v] = cpui386_get_cycle(cpu);
			cpui386_delete(cpu);
		}
		int same = !memcmp(st[0], st[1], sizeof st[0]) && !memcmp(ma, mb, size);
		printf("R %d %s: %ld insns, stop %d/%d exc %d/%d eip %08x/%08x\n", k, same ? "same" : "DIFF",
		       n[1], stop[0], stop[1], exc[0], exc[1], (unsigned)st[0][10], (unsigned)st[1][10]);
		if (!same) {
			for (int i = 0; i < 14; i++)
				if (st[0][i] != st[1][i])
					printf("  st[%d] kernel %08x C %08x\n", i, (unsigned)st[0][i], (unsigned)st[1][i]);
			for (uint32_t i = 0; i < size; i++)
				if (ma[i] != mb[i]) {
					printf("  mem %06x kernel %02x C %02x\n", (unsigned)i, ma[i], mb[i]);
					break;
				}
		}
	}
}
#endif

/* ---- main -------------------------------------------------------------- */

static inline void mem_slack_clear(void) { memset(mem + MEM_SIZE, 0, 65536); }

#ifndef CPUBENCH_ARM
static uint8_t *read_file(const char *path, unsigned *size)
{
	FILE *f = fopen(path, "rb");
	if (!f) { perror(path); exit(1); }
	fseek(f, 0, SEEK_END);
	*size = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t *p = malloc(*size);
	if (fread(p, 1, *size, f) != *size) exit(1);
	fclose(f);
	return p;
}

int main(int argc, char **argv)
{
	/* (+64 KB: the core walks "page 0x100000" for an access that wraps at 4 GB) */
	mem = aligned_alloc(4096, MEM_SIZE + 65536);
	memset(mem, 0, MEM_SIZE + 65536);
	if (argc >= 3 && !strcmp(argv[1], "bench")) {
		int bad = 0;
		for (int i = 2; i < argc; i++) {
			unsigned size;
			uint8_t *img = read_file(argv[i], &size);
			const char *name = strrchr(argv[i], '/');
			name = name ? name + 1 : argv[i];
			char nm[32];
			snprintf(nm, sizeof nm, "%s", name);
			char *dot = strchr(nm, '.');
			if (dot) *dot = 0;
			bad |= bench(nm, img, size, 0);
			free(img);
		}
		return bad;
	}
	if (argc >= 3 && !strcmp(argv[1], "bench16")) {
		int bad = 0;
		for (int i = 2; i < argc; i++) {
			unsigned size;
			uint8_t *img = read_file(argv[i], &size);
			bad |= bench16(argv[i], img, size);
			free(img);
		}
		return bad;
	}
	if (argc >= 4 && !strcmp(argv[1], "fuzz"))
		return fuzz(strtoull(argv[2], NULL, 0), 0, strtoul(argv[3], NULL, 0), argc > 4);
	if (argc >= 4 && !strcmp(argv[1], "fuzzk")) {
		kmode = 1;
		return fuzz(strtoull(argv[2], NULL, 0), 0, strtoul(argv[3], NULL, 0), argc > 4);
	}
	if (argc >= 5 && !strcmp(argv[1], "fuzzfrom"))  /* (SEED START COUNT: a case after others) */
		return fuzz(strtoull(argv[2], NULL, 0), strtoul(argv[3], NULL, 0), strtoul(argv[4], NULL, 0), argc > 5);
	if (argc >= 4 && !strcmp(argv[1], "fuzzone"))
		return fuzz(strtoull(argv[2], NULL, 0), strtoul(argv[3], NULL, 0), 1, argc > 4 ? 2 : 1);
	fprintf(stderr, "usage: %s bench IMAGE... | fuzz SEED COUNT [v]\n", argv[0]);
	return 2;
}
#else
int main(void)
{
	arm_init();
	extern const char *const arm_bench_names[];
	int bad = 0;
#ifdef ARM_REPLAY
	replay(ARM_REPLAY);
	return 0;
#endif
#ifdef ARM_FUZZ
	/* (run.sh armfuzz: the same cases as the host's, compared) */
#ifdef ARM_FUZZ_K
	kmode = 1;
#endif
	mem_slack_clear();
#ifndef ARM_FUZZ_START
#define ARM_FUZZ_START 0
#define ARM_FUZZ_V 0
#endif
	int r = fuzz(ARM_FUZZ, ARM_FUZZ_START, ARM_FUZZ_N, ARM_FUZZ_V);
#if JIT16
	{
		extern uint32_t jit_stat_runs, jit_stat_insns, jit_stat_blocks, jit_stat_flushes, jit_stat_bails;
		printf("J runs %lu insns %lu blocks %lu flushes %lu bails %lu\n", (unsigned long)jit_stat_runs,
		       (unsigned long)jit_stat_insns, (unsigned long)jit_stat_blocks, (unsigned long)jit_stat_flushes,
		       (unsigned long)jit_stat_bails);
	}
#endif
	return r;
#endif
	for (int i = 0; arm_bench_names[i]; i++) {
		unsigned size;
		const unsigned char *img = arm_image(arm_bench_names[i], &size);
#ifdef ARM_BENCH16
		bad |= bench16(arm_bench_names[i], img, size);
#else
		bad |= bench(arm_bench_names[i], img, size, 0);
#endif
#if JIT16
		/* (each image's own: the counters from zero) */
		extern uint32_t jit_stat_runs, jit_stat_insns, jit_stat_blocks, jit_stat_flushes, jit_stat_bails,
			jit_stat_invals;
		printf("J runs %lu insns %lu blocks %lu flushes %lu bails %lu invals %lu\n", (unsigned long)jit_stat_runs,
		       (unsigned long)jit_stat_insns, (unsigned long)jit_stat_blocks, (unsigned long)jit_stat_flushes,
		       (unsigned long)jit_stat_bails, (unsigned long)jit_stat_invals);
		jit_stat_runs = jit_stat_insns = jit_stat_blocks = jit_stat_flushes = jit_stat_bails = jit_stat_invals = 0;
#endif
	}
	extern void arm_prof_dump(void);
	arm_prof_dump();
#ifdef FX_KSTATS
	{
		/* (fastloop.inc's FX_KSTATS: what the kernel left to the C loop) */
		extern unsigned fx_kstats[0x200];
		for (int i = 0; i < 0x200; i++)
			if (fx_kstats[i] > 2000)
				printf("K %03x %u\n", i, fx_kstats[i]);
	}
#endif
	return bad;
}
#endif
