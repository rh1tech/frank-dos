/*
 * FRANK DOS — the native BIOS: what its handlers share.
 *
 * The guest reaches a handler through a trap in the ROM (rom.asm). A
 * handler sees the CPU as the INT left it: the registers are the caller's,
 * SS:SP holds the interrupt frame (IP, CS, FLAGS), and the result flags go
 * into that frame's FLAGS, which the stub's IRET restores (hle_cf/hle_zf).
 *
 * Guest memory is plain host memory (pc->phys_mem, in PSRAM) except the VGA
 * window A0000h-BFFFFh, which goes through the VGA's planes; g_rd/g_wr
 * take either.
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: MIT
 */
#ifndef HLE_H
#define HLE_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "pc.h"
#include "hle/rom_layout.h"
#include "hle/hle_mem.h"

extern PC *hle_pc;

/* ---- registers ---------------------------------------------------------- */
enum { HS_ES, HS_CS, HS_SS, HS_DS, HS_FS, HS_GS };
enum { HR_AX, HR_CX, HR_DX, HR_BX, HR_SP, HR_BP, HR_SI, HR_DI };

#define R16(c, r)  ((c)->gprx[r].r16)
#define R8L(c, r)  ((c)->gprx[r].r8[0])
#define R8H(c, r)  ((c)->gprx[r].r8[1])
#define AX  R16(cpu, HR_AX)
#define BX  R16(cpu, HR_BX)
#define CX  R16(cpu, HR_CX)
#define DX  R16(cpu, HR_DX)
#define SI  R16(cpu, HR_SI)
#define DI  R16(cpu, HR_DI)
#define BP  R16(cpu, HR_BP)
#define SP  R16(cpu, HR_SP)
#define AL  R8L(cpu, HR_AX)
#define AH  R8H(cpu, HR_AX)
#define BL  R8L(cpu, HR_BX)
#define BH  R8H(cpu, HR_BX)
#define CL  R8L(cpu, HR_CX)
#define CH  R8H(cpu, HR_CX)
#define DL  R8L(cpu, HR_DX)
#define DH  R8H(cpu, HR_DX)

bool cpui386_load_seg(CPUI386 *cpu, int seg, int sel);   /* i386.c: a real-mode segment load */
static inline uint16_t seg_get(CPUI386 *cpu, int s) { return (uint16_t)cpu->seg[s].sel; }
/* a real-mode segment load (never CS: the ROM's far jumps do that) */
static inline void seg_set(CPUI386 *cpu, int s, uint16_t v)
{
	cpu->seg[s].sel = v;
	cpu->seg[s].base = (uint32_t)v << 4;
}
#define ES seg_get(cpu, HS_ES)
#define DS seg_get(cpu, HS_DS)
#define SS seg_get(cpu, HS_SS)
#define LIN(seg, off) (((uint32_t)(seg) << 4) + (uint16_t)(off))

/* the BIOS data area */
#define BDA(off) (0x400u + (off))

/* ---- the interrupt frame's flags ------------------------------------------ */
static inline uint32_t hle_frame(CPUI386 *cpu) { return LIN(SS, SP); }
static inline void hle_flag(CPUI386 *cpu, uint16_t bit, bool on)
{
	uint32_t f = hle_frame(cpu) + 4;
	uint16_t v = g_rd16(f);
	g_wr16(f, on ? (v | bit) : (v & ~bit));
}
#define hle_cf(cpu, on) hle_flag(cpu, 0x0001, on)
#define hle_zf(cpu, on) hle_flag(cpu, 0x0040, on)

/* ---- handlers ------------------------------------------------------------- */
enum hle_result {
	HLE_DONE,       /* on to the stub's IRET (trap + 4) */
	HLE_WAIT,       /* the stub's wait loop: STI, HLT, the trap again */
	HLE_JUMPED,     /* next_ip set by the handler */
};
typedef enum hle_result (*hle_fn)(CPUI386 *cpu);

void hle_bios_install(PC *pc);       /* the trap hook, the ROM image at F0000h */
void hle_report(void);               /* the console's ~C regs~ */
void hle_screen(void);               /* the console's ~C screen~ */

/* the handlers (int10.c, int13.c, kbd.c, misc.c) */
enum hle_result hle_int08(CPUI386 *cpu);
enum hle_result hle_int09(CPUI386 *cpu);
enum hle_result hle_int10(CPUI386 *cpu);
enum hle_result hle_int11(CPUI386 *cpu);
enum hle_result hle_int12(CPUI386 *cpu);
enum hle_result hle_int13(CPUI386 *cpu);
enum hle_result hle_int14(CPUI386 *cpu);
enum hle_result hle_int15(CPUI386 *cpu);
enum hle_result hle_int16(CPUI386 *cpu);
enum hle_result hle_int17(CPUI386 *cpu);
enum hle_result hle_int18(CPUI386 *cpu);
enum hle_result hle_int19(CPUI386 *cpu);
enum hle_result hle_int1a(CPUI386 *cpu);
enum hle_result hle_irq12(CPUI386 *cpu);     /* misc.c: the PS/2 mouse */

/* POST's parts */
void int10_post(PC *pc);             /* mode 03h, fonts in the ROM */
void int13_post(PC *pc);             /* the disk images (config.ini) */
void kbd_post(void);
void misc_post(void);

/* INT 10h's teletype, for the BIOS's own messages */
void int10_tty(uint8_t ch);
void int10_puts(const char *s);

/* the disk images: from the config before pc_new() (which must not open them) */
void int13_set_images(const char *dir, const char *fda, const char *fdb,
		      const char *hda, const char *hdb);
int int13_boot_drive(void);          /* -1: none */

/* port I/O as the guest makes it */
static inline void outb(int port, uint8_t v) { hle_pc->cpu->cb.io_write8(hle_pc->cpu->cb.io, port, v); }
static inline uint8_t inb(int port) { return hle_pc->cpu->cb.io_read8(hle_pc->cpu->cb.io, port); }

#endif
