/*
 * FRANK DOS — the FreeDOS kernel port's view of the machine.
 *
 * DnCraptor's native FreeDOS (src/fdos, from murm386) is written against
 * murm386's CPU ABI (286/cpu.h, i386.h, mem.h, bios/bios.h): register and
 * segment macros, the flags as bitfields (cf = 1), plain guest memory
 * accessors, the native BIOS functions, and a nested-execution model in
 * which native code runs guest code with "while (!done) pc_step(pc, n)".
 *
 * Here those map onto frank-dos:
 *  - registers: the core's gprx[] and seg[] directly;
 *  - flags: the core's cpu->flags, which is exact while native code runs
 *    (every entry into the kernel folds the lazy flags first, dosrt.c);
 *  - memory: the flat guest RAM (hle/hle_mem.h);
 *  - nested execution: the kernel runs on coroutines (dosrt.c); pc_step()
 *    inside it yields one CPU batch to the main loop.
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: MIT
 */
#ifndef FDOS_PORT_H
#define FDOS_PORT_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__cplusplus) && !defined(_Static_assert)
#define _Static_assert static_assert
#endif

/* murm386's section placement for the kernel (flash): plain here */
#ifndef __hfa_func
#define __hfa(group)
#define __hfa_func(func_name) func_name
#endif
#ifndef __STRING
#define __STRING(x) #x
#endif

#include "i386.h"               /* the core (src/i386.h) */
#include "hle/hle_mem.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct CPUI386 CPU;
struct PC;
extern struct PC *pc;

/* ---- registers ------------------------------------------------------------ */
#define regax 0
#define regcx 1
#define regdx 2
#define regbx 3
#define regsp 4
#define regbp 5
#define regsi 6
#define regdi 7
#define reges 0
#define regcs 1
#define regss 2
#define regds 3
#define regfs 4
#define reggs 5

enum { SEG_ES = 0, SEG_CS, SEG_SS, SEG_DS, SEG_FS, SEG_GS, SEG_LDT, SEG_TR };
enum {
	CF = 0x1, PF = 0x4, AF = 0x10, ZF = 0x40, SF = 0x80, TF = 0x100, IF = 0x200,
	DF = 0x400, OF = 0x800, IOPL = 0x3000, NT = 0x4000, RF = 0x10000, VM = 0x20000,
};

#define AL_REG_IDX 0
#define CL_REG_IDX 1
#define DL_REG_IDX 2
#define BL_REG_IDX 3
#define AH_REG_IDX 4
#define CH_REG_IDX 5
#define DH_REG_IDX 6
#define BH_REG_IDX 7
#define regal AL_REG_IDX
#define regah AH_REG_IDX
#define regcl CL_REG_IDX
#define regch CH_REG_IDX
#define regdl DL_REG_IDX
#define regdh DH_REG_IDX
#define regbl BL_REG_IDX
#define regbh BH_REG_IDX

#define CPU_AX    cpu->gprx[regax].r16
#define CPU_BX    cpu->gprx[regbx].r16
#define CPU_CX    cpu->gprx[regcx].r16
#define CPU_DX    cpu->gprx[regdx].r16
#define CPU_SI    cpu->gprx[regsi].r16
#define CPU_DI    cpu->gprx[regdi].r16
#define CPU_BP    cpu->gprx[regbp].r16
#define CPU_SP    cpu->gprx[regsp].r16
#define CPU_AL    cpu->gprx[regax].r8[0]
#define CPU_AH    cpu->gprx[regax].r8[1]
#define CPU_BL    cpu->gprx[regbx].r8[0]
#define CPU_BH    cpu->gprx[regbx].r8[1]
#define CPU_CL    cpu->gprx[regcx].r8[0]
#define CPU_CH    cpu->gprx[regcx].r8[1]
#define CPU_DL    cpu->gprx[regdx].r8[0]
#define CPU_DH    cpu->gprx[regdx].r8[1]

#define getreg16(regid) (cpu->gprx[regid].r16)
#define getreg32(regid) (cpu->gprx[regid].r32)
#define getreg8(i)  (cpu->gprx[(i) & 3].r8[(i) >> 2])
#define putreg16(regid, v) cpu->gprx[regid].r16 = (v)
#define putreg32(regid, v) cpu->gprx[regid].r32 = (v)
#define putreg8(i, v) (cpu->gprx[(i) & 3].r8[(i) >> 2] = (v))

/* a real-mode segment load (the core's own, for CS as well) */
bool cpui386_load_seg(CPU *cpu, int seg, int sel);
static inline uint16_t fdos_get_seg(const CPU *cpu, int s) { return (uint16_t)cpu->seg[s].sel; }
static inline void fdos_set_seg(CPU *cpu, int s, uint16_t v) { cpui386_load_seg(cpu, s, v); }

#define CPU_ES fdos_get_seg(cpu, SEG_ES)
#define CPU_CS fdos_get_seg(cpu, SEG_CS)
#define CPU_SS fdos_get_seg(cpu, SEG_SS)
#define CPU_DS fdos_get_seg(cpu, SEG_DS)
#define CPU_FS fdos_get_seg(cpu, SEG_FS)
#define CPU_GS fdos_get_seg(cpu, SEG_GS)
#define SET_ES(x) fdos_set_seg(cpu, SEG_ES, (x))
#define SET_CS(x) fdos_set_seg(cpu, SEG_CS, (x))
#define SET_SS(x) fdos_set_seg(cpu, SEG_SS, (x))
#define SET_DS(x) fdos_set_seg(cpu, SEG_DS, (x))
#define SET_FS(x) fdos_set_seg(cpu, SEG_FS, (x))
#define SET_GS(x) fdos_set_seg(cpu, SEG_GS, (x))
#define getsegreg(regid) fdos_get_seg(cpu, (regid))
#define putsegreg(regid, v) fdos_set_seg(cpu, (regid), (v))

#define PREFETCH_RESET cpu->prefetch_base = (u32)-1;
#define SET_IP(x) do { cpu->ip = (uint16_t)(x); cpu->next_ip = (uint16_t)(x); cpu->halt = false; PREFETCH_RESET } while (0)
#define CPU_IP    ((uint16_t)(cpu->next_ip))
#define StepIP(x) SET_IP(CPU_IP + (x))

/* ---- flags: bitfields over cpu->flags (exact: see dosrt.c) ------------------ */
typedef union {
	uint32_t value;
	struct {
		unsigned CF : 1, _1 : 1, PF : 1, _3 : 1, AF : 1, _5 : 1, ZF : 1, SF : 1;
		unsigned TF : 1, IF : 1, DF : 1, OF : 1, IOPL : 2, NT : 1, _15 : 1;
		unsigned RF : 1, VM : 1, AC : 1, VIF : 1, VIP : 1, ID : 1;
	} bits;
} __attribute__((may_alias)) x86_flags_t;

#define FDOS_FLAGS(c) (*(x86_flags_t *)&(c)->flags)
#define cf  FDOS_FLAGS(cpu).bits.CF
#define pf  FDOS_FLAGS(cpu).bits.PF
#define af  FDOS_FLAGS(cpu).bits.AF
#define zf  FDOS_FLAGS(cpu).bits.ZF
#define sf  FDOS_FLAGS(cpu).bits.SF
#define tf  FDOS_FLAGS(cpu).bits.TF
#define ifl FDOS_FLAGS(cpu).bits.IF
#define df  FDOS_FLAGS(cpu).bits.DF
#define of  FDOS_FLAGS(cpu).bits.OF
#define CPU_FL_CF cf
#define CPU_FL_ZF zf
#define CPU_FL_IFL ifl
#define CPU_FL_DF df

/* ---- memory ------------------------------------------------------------------ */
#define read86(a)        g_rd8(a)
#define readw86(a)       g_rd16(a)
#define readdw86(a)      g_rd32(a)
#define write86(a, v)    g_wr8((a), (uint8_t)(v))
#define writew86(a, v)   g_wr16((a), (uint16_t)(v))
#define writedw86(a, v)  g_wr32((a), (uint32_t)(v))
#define pload8(a)        g_rd8(a)
#define pload16(a)       g_rd16(a)
#define pload32(a)       g_rd32(a)
#define pstore8(a, v)    g_wr8((a), (uint8_t)(v))
#define pstore16(a, v)   g_wr16((a), (uint16_t)(v))
#define pstore32(a, v)   g_wr32((a), (uint32_t)(v))
#define segbase(x)       ((uint32_t)(x) << 4)
#define getmem8(x, y)    read86(segbase(x) + (uint16_t)(y))
#define getmem16(x, y)   readw86(segbase(x) + (uint16_t)(y))
#define getmem32(x, y)   readdw86(segbase(x) + (uint16_t)(y))
#define putmem8(x, y, z)  write86(segbase(x) + (uint16_t)(y), (z))
#define putmem16(x, y, z) writew86(segbase(x) + (uint16_t)(y), (z))
#define putmem32(x, y, z) writedw86(segbase(x) + (uint16_t)(y), (z))
#define PC_RAM hle_ram
#define NO_PAGING 1
#ifndef FRANK_PSRAM_BASE
#define FRANK_PSRAM_BASE 0x11000000u
#endif
#define PSRAM_BASE_ADDR ((uint8_t *)FRANK_PSRAM_BASE)  /* the guest RAM (pc.c), flat: a constant, valid before main() */
#define VGA_WINDOW(addr) g_is_vga(addr)
#define unlikely(x) __builtin_expect(!!(x), 0)
#define likely(x) __builtin_expect(!!(x), 1)

/* a host pointer for a span of guest memory (plain RAM; the VGA window has none) */
static inline uint8_t *guest_span_ptr_ex(uint32_t addr, uint32_t *span, bool write_access)
{
	(void)write_access;
	/* (none in the VGA window, A0000-BFFFF: g_rd8/g_wr8 there) */
	*span = addr < 0xA0000u ? 0xA0000u - addr : (addr >= 0xC0000u && addr < hle_ram_size ? hle_ram_size - addr : 0);
	return hle_ram + addr;
}
static inline uint8_t *guest_span_ptr(uint32_t addr, uint32_t *span) { return guest_span_ptr_ex(addr, span, true); }
static inline const uint8_t *guest_span_ptr_read(uint32_t addr, uint32_t *span) { return guest_span_ptr_ex(addr, span, false); }
static inline void guest_read_block(uint32_t src, void *dst, size_t len) { g_read(dst, src, (uint32_t)len); }
static inline void guest_write_block(uint32_t dst, const void *src, size_t len) { g_write(dst, src, (uint32_t)len); }
static inline void guest_fill_block(uint32_t dst, uint8_t value, size_t len)
{
	if (dst + len <= 0xA0000u || (dst >= 0xC0000u && dst + len <= hle_ram_size)) {
		memset(hle_ram + dst, value, len);
		jit_note_write(dst, (uint32_t)len);
		return;
	}
	while (len--)
		g_wr8(dst++, value);
}
static inline size_t guest_find_byte(uint32_t src, uint8_t value, size_t len)
{
	for (size_t i = 0; i < len; i++)
		if (g_rd8(src + (uint32_t)i) == value)
			return i;
	return SIZE_MAX;
}
static inline size_t guest_strnlen_block(uint32_t src, size_t maxlen)
{
	size_t off = guest_find_byte(src, 0, maxlen);
	return off == SIZE_MAX ? maxlen : off;
}
static inline void guest_move_block(uint32_t dst, uint32_t src, size_t len)
{
	if (dst == src || len == 0)
		return;
	if (dst > src && dst - src < len) {
		while (len--)
			g_wr8(dst + (uint32_t)len, g_rd8(src + (uint32_t)len));
		return;
	}
	for (size_t i = 0; i < len; i++)
		g_wr8(dst + (uint32_t)i, g_rd8(src + (uint32_t)i));
}
static inline bool pstore_block(uint32_t dst, uint32_t src, int len)
{
	guest_move_block(dst, src, (size_t)len);
	return true;
}
size_t strnlen(const char *s, size_t n);

/* the guest's RAM, for DOS and XMS (the native arena is past it: dosrt.h),
 * less 64 KB: the kernel ends a native application's stack at the top of
 * this, and a murm386 application writes past its stack's top (Doom's EZ
 * build copies 16 KB to 1 KB below it); there it hits nothing */
#define FDOS_TOP_GUARD (64u * 1024u)
#define phys_mem_size ((unsigned long)(hle_ram_size - FDOS_TOP_GUARD))

/* murm386 lends free PSRAM to its SD driver as a sector cache: not here */
enum { SDCARD_FF_QSPI_OWNER_XMS = 1, SDCARD_FF_QSPI_OWNER_NATIVE = 2 };
static inline void sdcard_enable_ff_dos_cache(void *base, size_t size) { (void)base; (void)size; }
static inline void sdcard_ff_qspi_cache_set_floor(int owner, const void *floor) { (void)owner; (void)floor; }
void *dos_api_memcpy(void *dst, const void *src, size_t len);
const void *dos_api_memchr(const void *src, int value, size_t len);
void *nf_memset(void *ptr, int value, size_t len);

/* ---- ports --------------------------------------------------------------------- */
#define cpu_portout8(p, v)  cpu->cb.io_write8(cpu->cb.io, (p), (v))
#define cpu_portout16(p, v) cpu->cb.io_write16(cpu->cb.io, (p), (v))
#define cpu_portin8(p)      (cpu->cb.io_read8(cpu->cb.io, (p)))
#define cpu_portin16(p)     (cpu->cb.io_read16(cpu->cb.io, (p)))

/* ---- registers saved around guest calls ------------------------------------------ */
typedef union {
	u32 r32;
	u16 r16;
	u8 r8[2];
} gprx_t;

typedef struct CPU_regs {
	gprx_t gprx[8];
	x86_flags_t flags;
	u16 es, ds, fs, gs;
} CPU_regs;
void cpu_save_regs(const CPU *cpu, CPU_regs *r);
void cpu_restore_regs(CPU *cpu, const CPU_regs *r);

/* ---- native <-> guest calls (dosrt.c) ------------------------------------------- */
struct bios_callback_params;
typedef bool (*bios_callback_t)(CPU *cpu, struct bios_callback_params *params);
typedef struct bios_callback_params {
	bios_callback_t callback;
	void *data;
	u16 expected_cs;
	u16 expected_ip;
	struct bios_callback_params *chain;
	bool done;
	const char *owner;
} bios_callback_params_t;

extern bool hle_native_done;        /* stop the guest: back to the kernel (their cpu->native_done) */
bool set_bios_callback(CPU *cpu, bios_callback_params_t *p, bool reenter);
bool drop_bios_callback(CPU *cpu, bios_callback_params_t *p);
void bios_intcall(CPU *cpu, uint8_t intnum, const char *owner);
void cpu_intcall(CPU *cpu, uint8_t intnum);     /* the INT's frame and jump, no wait */
void fdos_pc_step(void);                        /* one CPU batch, from the kernel */
#define pc_step(p, n) fdos_pc_step()
void pc_service(struct PC *p);

static inline bool cpu_pending_trap(void) { return false; }
static inline void cpu_pending_trap_set(bool v) { (void)v; }
static inline bool cpu_irq_shadow(const CPU *c) { (void)c; return false; }
static inline void cpu_irq_shadow_set(CPU *c, bool v) { (void)c; (void)v; }

/* the native stubs in the ROM (hle/rom.asm: the DOS entries at F000:D000) */
#define NATIVE_BIOS_STUB_SEG   0xF000u
#define NATIVE_BIOS_STUB_OFF   0xD000u
#define NATIVE_BIOS_STUB_SIZE  8u
#define NATIVE_BIOS_STUB_OFF_FOR(id) ((uint16_t)(NATIVE_BIOS_STUB_OFF + NATIVE_BIOS_STUB_SIZE * (uint8_t)(id)))
#define NATIVE_BIOS_CALLBACK_PHYS 0xFFEFFu
bool fdos_vector_is_native(uint8_t intnum);     /* IVT[n] still the ROM's own */

typedef bool (*handler_t)(CPU *);
void cpu_install_dos_handlers(CPU *cpu);

/* the BIOS functions the kernel calls directly: on the CPU as an INT would leave
 * it, the result flags in cpu->flags (bios_shim.c) */
bool bios_10h(CPU *cpu);
bool bios_11h(CPU *cpu);
bool bios_12h(CPU *cpu);
bool bios_13h(CPU *cpu);
bool bios_14h(CPU *cpu);
bool bios_15h(CPU *cpu);
bool bios_16h(CPU *cpu);
bool bios_17h(CPU *cpu);
bool bios_1Ah(CPU *cpu);
bool bios_teletype(CPU *cpu, uint8_t ch, uint8_t page);
void bios_puts(CPU *cpu, const char *s);
void bios_printf(CPU *cpu, const char *fmt, ...);
void cpu_err_msg(CPU *cpu, const char *msg);

static inline void print_line2(const char *s, int row, int col)
{
	uint32_t base = 0xB8000 + row * 160;
	col <<= 1;
	for (uint16_t c; (c = (uint8_t)*s) && col < 160; ++s, col += 2)
		g_wr16(base + col, 0x0F00 | c);
}
static inline void print_line(const char *s, int row) { print_line2(s, row, 0); }

static inline uint8_t cmos_read(CPU *cpu, uint8_t reg)
{
	cpu->cb.io_write8(cpu->cb.io, 0x70, reg);
	return cpu->cb.io_read8(cpu->cb.io, 0x71);
}
static inline void cmos_write(CPU *cpu, uint8_t reg, uint8_t val)
{
	cpu->cb.io_write8(cpu->cb.io, 0x70, reg);
	cpu->cb.io_write8(cpu->cb.io, 0x71, val);
}

uint32_t get_uticks(void);

#ifdef __cplusplus
}
#endif

#endif
