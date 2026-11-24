/*
 * FRANK DOS — the small BIOS services: the timer tick (INT 08h's native
 * half), equipment (11h), memory (12h, 15h), serial and printer (14h, 17h:
 * none), the system services (15h), the clock (1Ah).
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: MIT
 */
#include "pico/time.h"
#include "hle/hle.h"

#define T_TICKS     BDA(0x6C)
#define T_MIDNIGHT  BDA(0x70)
#define T_MOTOR     BDA(0x40)
#define TICKS_DAY   0x1800B0u

static uint8_t cmos(uint8_t r) { outb(0x70, r); return inb(0x71); }
static uint8_t bcd2bin(uint8_t v) { return (v >> 4) * 10 + (v & 15); }

void int13_tick(void);                  /* int13.c: written images flushed */

enum hle_result hle_int08(CPUI386 *cpu)
{
	(void)cpu;
	int13_tick();
	uint32_t t = g_rd32(T_TICKS) + 1;
	if (t >= TICKS_DAY) {
		t = 0;
		g_wr8(T_MIDNIGHT, 1);
	}
	g_wr32(T_TICKS, t);
	uint8_t m = g_rd8(T_MOTOR);
	if (m)
		g_wr8(T_MOTOR, m - 1);
	return HLE_DONE;
}

enum hle_result hle_int11(CPUI386 *cpu) { AX = g_rd16(BDA(0x10)); return HLE_DONE; }
enum hle_result hle_int12(CPUI386 *cpu) { AX = g_rd16(BDA(0x13)); return HLE_DONE; }
enum hle_result hle_int14(CPUI386 *cpu) { AX = 0x8000; return HLE_DONE; }    /* no port: timeout */
enum hle_result hle_int17(CPUI386 *cpu) { AH = 0x01; return HLE_DONE; }      /* no printer: timeout */

/* ---- INT 15h ---------------------------------------------------------------- */
static uint32_t ext_kb(void)                /* memory above 1 MB, in KB */
{
	return hle_ram_size > 0x100000u ? (hle_ram_size - 0x100000u) / 1024 : 0;
}

static enum hle_result unsupported(CPUI386 *cpu)
{
	AH = 0x86;
	hle_cf(cpu, true);
	return HLE_DONE;
}

/* AH=87h: CX words between the descriptors at ES:SI+10h and +18h */
static void block_move(CPUI386 *cpu)
{
	uint32_t gdt = LIN(ES, SI);
	uint32_t src = g_rd16(gdt + 0x12) | (uint32_t)g_rd8(gdt + 0x14) << 16 | (uint32_t)g_rd8(gdt + 0x17) << 24;
	uint32_t dst = g_rd16(gdt + 0x1A) | (uint32_t)g_rd8(gdt + 0x1C) << 16 | (uint32_t)g_rd8(gdt + 0x1F) << 24;
	uint32_t n = (uint32_t)CX * 2;
	uint8_t buf[256];
	while (n) {
		uint32_t k = n < sizeof buf ? n : sizeof buf;
		g_read(buf, src, k);
		g_write(dst, buf, k);
		src += k;
		dst += k;
		n -= k;
	}
}

/* AH=86h: a wait of CX:DX microseconds, in the stub's HLT loop */
static enum hle_result wait_us(CPUI386 *cpu)
{
	static bool waiting;
	static uint32_t frame;
	static uint64_t until;
	uint32_t f = hle_frame(cpu);
	if (!waiting || frame != f) {
		waiting = true;
		frame = f;
		until = time_us_64() + ((uint32_t)CX << 16 | DX);
	}
	if (time_us_64() < until)
		return HLE_WAIT;
	waiting = false;
	AH = 0;
	hle_cf(cpu, false);
	return HLE_DONE;
}

/* AX=E820h: the memory map, one entry per call (EBX the next) */
static enum hle_result e820(CPUI386 *cpu)
{
	static const struct { uint32_t base, len, type; } fixed[] = {
		{0x00000000, 0x0009FC00, 1},
		{0x0009FC00, 0x00000400, 2},
		{0x000E0000, 0x00020000, 2},
	};
	uint32_t i = cpu->gprx[HR_BX].r32;
	if (cpu->gprx[HR_DX].r32 != 0x534D4150 || i > 3)
		return unsupported(cpu);
	uint32_t base, len, type;
	if (i < 3) {
		base = fixed[i].base;
		len = fixed[i].len;
		type = fixed[i].type;
	} else {
		base = 0x100000;
		len = hle_ram_size - 0x100000;
		type = 1;
	}
	uint32_t p = LIN(ES, DI);
	g_wr32(p, base); g_wr32(p + 4, 0);
	g_wr32(p + 8, len); g_wr32(p + 12, 0);
	g_wr32(p + 16, type);
	cpu->gprx[HR_AX].r32 = 0x534D4150;
	cpu->gprx[HR_CX].r32 = 20;
	cpu->gprx[HR_BX].r32 = i < 3 ? i + 1 : 0;
	hle_cf(cpu, false);
	return HLE_DONE;
}

/* ---- the PS/2 mouse: INT 15h C2h and IRQ 12 -----------------------------------
 * Through the emulated 8042's ports, as a BIOS does: a command to the mouse
 * is D4h to port 64h, then the byte to 60h; its answers come back in the
 * output buffer with the AUX bit (20h) in the status. IRQ 12's stub (rom.asm)
 * hands each byte to hle_irq12; a whole 3-byte packet goes to the handler
 * INT 15h C207h set (its far pointer at F000:ROM_MOUSE_FN). */
static struct {
	bool enabled;
	uint8_t pkt[3], n;
} ms;

static int aux_read(void)
{
	for (int i = 0; i < 64; i++) {
		uint8_t st = inb(0x64);
		if ((st & 0x21) == 0x21)
			return inb(0x60);
		if (st & 0x01)                  /* (a keyboard byte first: not ours) */
			return -1;
	}
	return -1;
}

static int aux_cmd(uint8_t b)
{
	outb(0x64, 0xD4);
	outb(0x60, b);
	return aux_read();                  /* FAh: acknowledged */
}

static void aux_irq(bool on)
{
	/* the controller's command byte, written, not read: its answer comes
	 * back as a keyboard byte, and the IRQ 1 it raises read it later as a
	 * keystroke (47h: Home). 47h: both interrupts, the system flag, scan
	 * code translation; 65h: the mouse's interrupt and clock off */
	outb(0x64, 0x60);
	outb(0x60, on ? 0x47 : 0x65);
	outb(0x64, on ? 0xA8 : 0xA7);       /* the mouse's interface */
	if (on) {
		outb(0xA1, inb(0xA1) & ~0x10);  /* IRQ 12, and the cascade */
		outb(0x21, inb(0x21) & ~0x04);
	}
}

static bool mouse_fn_set(void)
{
	return g_rd16(ROM_BASE + ROM_MOUSE_FN) || g_rd16(ROM_BASE + ROM_MOUSE_FN + 2);
}

static enum hle_result mouse_ok(CPUI386 *cpu)
{
	AH = 0;
	hle_cf(cpu, false);
	return HLE_DONE;
}

static enum hle_result mouse_err(CPUI386 *cpu, uint8_t code)
{
	AH = code;                          /* 1 invalid function, 2 input, 3 interface, 5 no handler */
	hle_cf(cpu, true);
	return HLE_DONE;
}

static enum hle_result int15_mouse(CPUI386 *cpu)
{
	static const uint8_t rates[7] = { 10, 20, 40, 60, 80, 100, 200 };
	switch (AL) {
	case 0x00:                          /* BH 1: enable, 0: disable */
		if (BH > 1)
			return mouse_err(cpu, 1);
		if (BH && !mouse_fn_set())
			return mouse_err(cpu, 5);
		ms.n = 0;
		aux_irq(BH);
		if (aux_cmd(BH ? 0xF4 : 0xF5) != 0xFA)
			return mouse_err(cpu, 3);
		ms.enabled = BH;
		return mouse_ok(cpu);
	case 0x01:                          /* reset: BH the ID, BL AAh */
	case 0x05:                          /* initialize (BH the packet size: 3) */
		if (AL == 0x05 && BH != 3)
			return mouse_err(cpu, 2);
		ms.enabled = false;
		ms.n = 0;
		if (aux_cmd(0xFF) != 0xFA)
			return mouse_err(cpu, 3);
		BL = (uint8_t)aux_read();       /* AAh: passed */
		BH = (uint8_t)aux_read();       /* the ID */
		if (AL == 0x05)
			BX = 0;
		return mouse_ok(cpu);
	case 0x02:                          /* BH 0-6: 10-200 reports a second */
		if (BH > 6)
			return mouse_err(cpu, 2);
		aux_cmd(0xF3);
		aux_cmd(rates[BH]);
		return mouse_ok(cpu);
	case 0x03:                          /* BH 0-3: 1-8 counts a mm */
		if (BH > 3)
			return mouse_err(cpu, 2);
		aux_cmd(0xE8);
		aux_cmd(BH);
		return mouse_ok(cpu);
	case 0x04:                          /* the device's type */
		aux_cmd(0xF2);
		BH = (uint8_t)aux_read();
		return mouse_ok(cpu);
	case 0x06:                          /* BH 0: status (BL, CL, DL), 1: scaling 1:1, 2: 2:1 */
		if (BH == 0) {
			aux_cmd(0xE9);
			BL = (uint8_t)aux_read();
			CL = (uint8_t)aux_read();
			DL = (uint8_t)aux_read();
		} else if (BH <= 2) {
			aux_cmd(BH == 1 ? 0xE6 : 0xE7);
		} else {
			return mouse_err(cpu, 1);
		}
		return mouse_ok(cpu);
	case 0x07:                          /* ES:BX the handler (0:0: none) */
		g_wr16(ROM_BASE + ROM_MOUSE_FN, BX);
		g_wr16(ROM_BASE + ROM_MOUSE_FN + 2, ES);
		return mouse_ok(cpu);
	default:
		return mouse_err(cpu, 1);
	}
}

/* IRQ 12's byte: AL 1 when it ends a packet, with CX the status, BX X, DX Y */
enum hle_result hle_irq12(CPUI386 *cpu)
{
	AL = 0;
	uint8_t st = inb(0x64);
	if ((st & 0x21) != 0x21)
		return HLE_DONE;
	uint8_t b = inb(0x60);
	if (!ms.enabled || !mouse_fn_set())
		return HLE_DONE;
	if (ms.n == 0 && !(b & 0x08))       /* (a packet's first byte has bit 3: in step again) */
		return HLE_DONE;
	ms.pkt[ms.n++] = b;
	if (ms.n < 3)
		return HLE_DONE;
	ms.n = 0;
	CX = ms.pkt[0];
	BX = ms.pkt[1];
	DX = ms.pkt[2];
	AL = 1;
	return HLE_DONE;
}

enum hle_result hle_int15(CPUI386 *cpu)
{
	switch (AH) {
	case 0xC2:
		return int15_mouse(cpu);
	case 0x4F:                              /* keyboard intercept: pass it on */
		hle_cf(cpu, true);
		return HLE_DONE;
	case 0x85:
	case 0x90:
	case 0x91:
		AH = 0;
		hle_cf(cpu, false);
		return HLE_DONE;
	case 0x86:
		return wait_us(cpu);
	case 0x87:
		block_move(cpu);
		AH = 0;
		hle_cf(cpu, false);
		return HLE_DONE;
	case 0x88:
		AX = (uint16_t)(ext_kb() > 0xFFFF ? 0xFFFF : ext_kb());
		hle_cf(cpu, false);
		return HLE_DONE;
	case 0xC0:
		seg_set(cpu, HS_ES, ROM_SEG);
		BX = ROM_CONFIG_TABLE;
		AH = 0;
		hle_cf(cpu, false);
		return HLE_DONE;
	case 0x24:                              /* A20 */
		switch (AL) {
		case 0x00: cpu_set_a20(cpu, 0); AH = 0; break;
		case 0x01: cpu_set_a20(cpu, 1); AH = 0; break;
		case 0x02: AL = (uint8_t)cpu_get_a20(cpu); AH = 0; break;
		case 0x03: BX = 0x0003; AH = 0; break;
		default: return unsupported(cpu);
		}
		hle_cf(cpu, false);
		return HLE_DONE;
	case 0xE8:
		if (AL == 0x01) {                   /* E801h: 1-16 MB in KB, above in 64 KB */
			uint32_t kb = ext_kb(), lo = kb > 15360 ? 15360 : kb;
			uint32_t hi = kb > 15360 ? (kb - 15360) / 64 : 0;
			AX = CX = (uint16_t)lo;
			BX = DX = (uint16_t)hi;
			hle_cf(cpu, false);
			return HLE_DONE;
		}
		if (AL == 0x20)
			return e820(cpu);
		return unsupported(cpu);
	default:
		return unsupported(cpu);
	}
}

/* ---- INT 1Ah ----------------------------------------------------------------- */
enum hle_result hle_int1a(CPUI386 *cpu)
{
	switch (AH) {
	case 0x00: {
		uint32_t t = g_rd32(T_TICKS);
		CX = t >> 16;
		DX = (uint16_t)t;
		AL = g_rd8(T_MIDNIGHT);
		g_wr8(T_MIDNIGHT, 0);
		break;
	}
	case 0x01:
		g_wr32(T_TICKS, (uint32_t)CX << 16 | DX);
		g_wr8(T_MIDNIGHT, 0);
		break;
	case 0x02:
		CH = cmos(4);
		CL = cmos(2);
		DH = cmos(0);
		DL = 0;
		hle_cf(cpu, false);
		break;
	case 0x04:
		CH = cmos(0x32);
		CL = cmos(9);
		DH = cmos(8);
		DL = cmos(7);
		hle_cf(cpu, false);
		break;
	case 0x03:
	case 0x05:                              /* setting the RTC: ignored */
		hle_cf(cpu, false);
		break;
	default:
		hle_cf(cpu, true);
		break;
	}
	return HLE_DONE;
}

static void cmos_wr(uint8_t r, uint8_t v) { outb(0x70, r); outb(0x71, v); }

void misc_post(void)
{
	/* CMOS 17h/18h and 30h/31h: memory above 1 MB in KB, as an AT's POST
	 * leaves them (the kernel's XMS sizes its pool from 17h/18h) */
	uint32_t kb = ext_kb() > 0xFFFF ? 0xFFFF : ext_kb();
	cmos_wr(0x17, kb & 0xFF);
	cmos_wr(0x18, kb >> 8);
	cmos_wr(0x30, kb & 0xFF);
	cmos_wr(0x31, kb >> 8);
	/* equipment: 80x25 colour, the FPU if there is one (floppies: int13_post) */
	/* bit 2: a pointing device, the 8042's PS/2 mouse port, always there
	 * (whatever feeds it: a PS/2 or USB mouse, the NES pad, the console) */
	g_wr16(BDA(0x10), 0x0020 | (hle_pc->cpu->fpu ? 0x0002 : 0) | (hle_pc->mouse ? 0x0004 : 0));
	ms.enabled = false;
	ms.n = 0;
	/* the tick count from the RTC's time of day */
	uint32_t s = bcd2bin(cmos(4)) * 3600u + bcd2bin(cmos(2)) * 60u + bcd2bin(cmos(0));
	g_wr32(T_TICKS, (uint32_t)((uint64_t)s * TICKS_DAY / 86400u));
}
