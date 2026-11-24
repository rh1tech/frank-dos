/*
 * FRANK DOS — the native BIOS: the trap, guest memory, the ROM, POST.
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>

#include "hle/hle.h"
#include "hle/fonts/font8x8.h"
#include "hle/fonts/font8x14.h"
#include "font8x16.h"
#include "fdos/dosrt.h"

PC *hle_pc;
uint8_t *hle_ram;
uint32_t hle_ram_size;

extern const uint8_t hle_rom_image[65536];   /* rom.asm, assembled (rom_bin.c) */

/* ---- guest memory ----------------------------------------------------- */
uint8_t g_rd8_slow(uint32_t a)
{
	if (g_is_vga(a))
		return vga_mem_read(hle_pc->vga, a - 0xA0000u);
	return 0xFF;
}

void g_wr8_slow(uint32_t a, uint8_t v)
{
	if (g_is_vga(a))
		vga_mem_write(hle_pc->vga, a - 0xA0000u, v);
}

void g_read(void *dst, uint32_t a, uint32_t n)
{
	uint8_t *d = dst;
	if (a + n <= hle_ram_size && (a + n <= 0xA0000u || a >= 0xC0000u)) {
		memcpy(d, hle_ram + a, n);
		return;
	}
	while (n--)
		*d++ = g_rd8(a++);
}

void g_write(uint32_t a, const void *src, uint32_t n)
{
	const uint8_t *s = src;
	if (a + n <= hle_ram_size && (a + n <= 0xA0000u || a >= 0xC0000u)) {
		memcpy(hle_ram + a, s, n);
		jit_note_write(a, n);
		return;
	}
	while (n--)
		g_wr8(a++, *s++);
}

/* ---- the trap ------------------------------------------------------------ */
static enum hle_result hle_post(CPUI386 *cpu);

static enum hle_result unknown_trap(CPUI386 *cpu)
{
	printf("hle: unknown trap at %04x:%04x\n", (unsigned)seg_get(cpu, HS_CS), (unsigned)cpu->ip);
	return HLE_DONE;
}

static hle_fn trap_fn(unsigned id)
{
	switch (id) {
	case 0x08: return hle_int08;
	case 0x09: return hle_int09;
	case 0x10: return hle_int10;
	case 0x11: return hle_int11;
	case 0x12: return hle_int12;
	case 0x13: return hle_int13;
	case 0x14: return hle_int14;
	case 0x15: return hle_int15;
	case 0x16: return hle_int16;
	case 0x17: return hle_int17;
	case 0x18: return hle_int18;
	case 0x19: return hle_int19;
	case 0x1A: return hle_int1a;
	case 0x74: return hle_irq12;
	case TRAP_POST: return hle_post;
	default: return unknown_trap;
	}
}

static uint32_t trap_count[0x40];       /* (the BIOS INTs: 00h-3Fh) */

/* 0F FF <id>: cpu->ip at the 0F; the default next_ip is past the id */
static bool hle_trap(CPUI386 *cpu)
{
	uint32_t at = cpu->seg[HS_CS].base + cpu->ip;
	unsigned id = g_rd16(at + 2);
	if (id < 0x40)
		trap_count[id]++;
	/* the FreeDOS kernel's entries and its return sentinel (fdos/dosrt.c) */
	if (id >= TRAP_DOS && id < TRAP_DOS + 0x100)
		return dosrt_int(cpu, (uint8_t)(id - TRAP_DOS));
	if (id == TRAP_CALLBACK)
		return dosrt_callback(cpu);
	switch (trap_fn(id)(cpu)) {
	case HLE_DONE:
		cpu->next_ip = cpu->ip + 4;
		break;
	case HLE_WAIT:
		cpu->next_ip = cpu->ip + 5;     /* past the IRET: STI, HLT, back */
		break;
	case HLE_JUMPED:
		break;
	}
	return true;
}

/* ---- POST ---------------------------------------------------------------- */
static void rom_fonts(void)
{
	uint8_t *rom = hle_ram + ROM_BASE;
	for (int i = 0; i < 256 * 16; i++) {    /* font_8x16 has its left pixel in bit 0 */
		uint8_t b = font_8x16[i], r = 0;
		for (int k = 0; k < 8; k++)
			r |= ((b >> k) & 1) << (7 - k);
		rom[ROM_FONT8X16 + i] = r;
	}
	memcpy(rom + ROM_FONT8X14, font_8x14, 256 * 14);
	memcpy(rom + ROM_FONT8X8, font_8x8, 256 * 8);
	memcpy(rom + ROM_FONT8X8_LO, font_8x8, 128 * 8);
}

static void set_vec(int n, uint16_t seg, uint16_t off)
{
	g_wr16(n * 4, off);
	g_wr16(n * 4 + 2, seg);
}

static enum hle_result hle_post(CPUI386 *cpu)
{
	memset(hle_ram, 0, 0x500);              /* IVT, BDA */
	jit_note_write(0, 0x500);
	const uint8_t *rom = hle_ram + ROM_BASE;
	for (int n = 0; n < 256; n++) {
		uint16_t off = rom[ROM_VECTORS + 2 * n] | rom[ROM_VECTORS + 2 * n + 1] << 8;
		if (off)
			set_vec(n, ROM_SEG, off);
	}
	set_vec(0x1E, ROM_SEG, ROM_DPT);
	set_vec(0x1F, ROM_SEG, ROM_FONT8X8_HI);
	set_vec(0x43, ROM_SEG, ROM_FONT8X8);

	/* the BDA: no serial or parallel ports, 640 KB, the keyboard buffer */
	g_wr16(BDA(0x13), 640);
	g_wr16(BDA(0x1A), 0x1E);
	g_wr16(BDA(0x1C), 0x1E);
	g_wr16(BDA(0x80), 0x1E);
	g_wr16(BDA(0x82), 0x3E);
	g_wr8(BDA(0x96), 0x10);                 /* 101/102-key keyboard */

	/* the PICs as a BIOS leaves them: IRQ 0-7 at 08h, 8-15 at 70h */
	outb(0x20, 0x11); outb(0x21, 0x08); outb(0x21, 0x04); outb(0x21, 0x01);
	outb(0xA0, 0x11); outb(0xA1, 0x70); outb(0xA1, 0x02); outb(0xA1, 0x01);
	outb(0x21, 0xB8);                       /* IRQ 0, 1, 2, 6 */
	outb(0xA1, 0xFF);
	/* PIT channel 0: 18.2 Hz */
	outb(0x43, 0x36); outb(0x40, 0); outb(0x40, 0);

	misc_post();
	kbd_post();
	int10_post(hle_pc);
	int13_post(hle_pc);

	char line[96];
	snprintf(line, sizeof line, "FRANK DOS BIOS v%d.%02d, %lu KB\r\n\r\n",
		 FRANK_DOS_VERSION_MAJOR, FRANK_DOS_VERSION_MINOR, (unsigned long)(hle_ram_size / 1024));
	int10_puts(line);

	seg_set(cpu, HS_SS, 0x0030);            /* the boot stack, as IBM's */
	SP = 0x0100;
	return HLE_DONE;
}

/* the console's "~C regs~": where the guest is, what the BIOS has seen */
void hle_report(void)
{
	CPUI386 *cpu = hle_pc->cpu;
	printf("cs:ip %04x:%04x ss:sp %04x:%04x ds %04x es %04x ax %04x bx %04x cx %04x dx %04x fl %04x halt %d\n",
	       (unsigned)seg_get(cpu, HS_CS), (unsigned)cpu->ip, (unsigned)SS, (unsigned)SP, (unsigned)DS,
	       (unsigned)ES, AX, BX, CX, DX, (unsigned)cpu->flags, cpu->halt);
	printf("ticks %lu  kbd head %04x tail %04x  pic masks %02x %02x\n", (unsigned long)g_rd32(BDA(0x6C)),
	       g_rd16(BDA(0x1A)), g_rd16(BDA(0x1C)), inb(0x21), inb(0xA1));
	dosrt_report();
	printf("traps:");
	for (unsigned i = 0; i < 0x40; i++)
		if (trap_count[i])
			printf(" %02x:%lu", i, (unsigned long)trap_count[i]);
	printf("\n");
}

/* the console's "~C screen~": the text screen as text (mode 03h/07h), else the mode */
void hle_screen(void)
{
	uint8_t mode = g_rd8(BDA(0x49));
	if (mode > 3 && mode != 7) {
		printf("screen: graphics mode %02x\n", mode);
		return;
	}
	uint32_t base = (mode == 7 ? 0xB0000 : 0xB8000) + g_rd16(BDA(0x4E));
	int cols = g_rd16(BDA(0x4A)), rows = g_rd8(BDA(0x84)) + 1;
	printf("screen: mode %02x, %dx%d, page offset %04x\n", mode, cols, rows, g_rd16(BDA(0x4E)));
	char line[134];
	for (int r = 0; r < rows && r < 60; r++) {
		int n = 0;
		for (int c = 0; c < cols && c < 132; c++) {
			uint8_t ch = g_rd8(base + (r * cols + c) * 2);
			line[n++] = (ch >= 32 && ch < 127) ? (char)ch : (ch ? '.' : ' ');
		}
		while (n && line[n - 1] == ' ')
			n--;
		line[n] = 0;
		printf("|%s\n", line);
	}
}

void hle_bios_install(PC *pc)
{
	hle_pc = pc;
	hle_ram = (uint8_t *)pc->phys_mem;
	hle_ram_size = (uint32_t)pc->phys_mem_size;
	memcpy(hle_ram + ROM_BASE, hle_rom_image, 65536);
	rom_fonts();
	jit_note_write(ROM_BASE, 65536);
	pc->cpu->hle_trap = hle_trap;
}
