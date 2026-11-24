/*
 * FRANK DOS — the BIOS functions the FreeDOS kernel calls directly
 * (bios_13h(cpu) and the rest, murm386's names), on frank-dos's native
 * BIOS (src/hle).
 *
 * Our handlers return their flags in the interrupt frame at SS:SP+4, as a
 * real BIOS does. A direct call has no frame, so each gets a private one
 * below SP (as murm386's dsk.cpp builds for itself), and its CF/ZF land in
 * the live flags, where the kernel looks for them. A handler that would
 * block (INT 16h with no key) yields to the guest until it can finish.
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: MIT
 */
#include <stdarg.h>
#include <stdio.h>

#include "hle/hle.h"
#include "fdos/dosrt.h"

void fdos_pc_step(void);

static bool direct(CPUI386 *cpu, hle_fn fn)
{
	uint16_t fl = (uint16_t)cpu_getflags(cpu);
	SP -= 6;
	uint32_t f = LIN(SS, SP);
	g_wr16(f, (uint16_t)cpu->ip);
	g_wr16(f + 2, seg_get(cpu, HS_CS));
	g_wr16(f + 4, fl);
	while (fn(cpu) == HLE_WAIT && dosrt_active())
		fdos_pc_step();
	uint16_t out = g_rd16(LIN(SS, SP) + 4);
	SP += 6;
	cpu_setflags(cpu, out & 0x41u, ~out & 0x41u);
	return true;
}

bool bios_10h(CPUI386 *cpu) { return direct(cpu, hle_int10); }
bool bios_11h(CPUI386 *cpu) { return direct(cpu, hle_int11); }
bool bios_12h(CPUI386 *cpu) { return direct(cpu, hle_int12); }
bool bios_13h(CPUI386 *cpu) { return direct(cpu, hle_int13); }
bool bios_14h(CPUI386 *cpu) { return direct(cpu, hle_int14); }
bool bios_15h(CPUI386 *cpu) { return direct(cpu, hle_int15); }
bool bios_16h(CPUI386 *cpu) { return direct(cpu, hle_int16); }
bool bios_17h(CPUI386 *cpu) { return direct(cpu, hle_int17); }
bool bios_1Ah(CPUI386 *cpu) { return direct(cpu, hle_int1a); }

bool bios_teletype(CPUI386 *cpu, uint8_t ch, uint8_t page)
{
	(void)cpu;
	(void)page;
	int10_tty(ch);
	return true;
}

void bios_puts(CPUI386 *cpu, const char *s)
{
	(void)cpu;
	for (; *s; s++) {
		if (*s == '\n')
			int10_tty('\r');
		int10_tty((uint8_t)*s);
	}
}

void bios_printf(CPUI386 *cpu, const char *fmt, ...)
{
	char buf[256];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	bios_puts(cpu, buf);
}

void cpu_err_msg(CPUI386 *cpu, const char *msg)
{
	printf("fdos: %s at %04x:%04x ax %04x bx %04x cx %04x dx %04x ss:sp %04x:%04x\n", msg,
	       seg_get(cpu, HS_CS), (unsigned)cpu->ip, AX, BX, CX, DX, SS, SP);
}

/* the console, for the kernel's diagnostics (its headers redefine printf) */
int frank_printf(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int n = vprintf(fmt, ap);
	va_end(ap);
	return n;
}
