/*
 * FRANK DOS host build: a stand-in for a native (EZ/ELF) application's code,
 * which the host cannot run. The kernel loads the application as on the
 * board, and where it would call its entry, it calls host_native_app()
 * instead (task.c). That makes the calls the application runtime makes
 * (apps/api/dos-api-sdtfn.c: int386() over the system table's slots 0 and 2),
 * so the ABI and the kernel's EXEC context behave as on the board.
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: MIT
 */
#include <string.h>

#include "../apps/api/pc.h"
#include "../apps/api/dos.h"

extern unsigned long dos_api_table_ptrs[];  /* src/ezabi/dos_api.c */
extern int frank_printf(const char *fmt, ...);

enum { SEG_ES = 0, SEG_CS = 1, SEG_SS = 2, SEG_DS = 3 };

static PC *app_get_PC(void)
{
	return ((PC *(*)(void))dos_api_table_ptrs[0])();
}

static void app_bios_intcall(CPU *cpu, uint8_t intnum, const char *owner)
{
	((void (*)(CPU *, uint8_t, const char *))dos_api_table_ptrs[2])(cpu, intnum, owner);
}

/* the runtime's int386_core(), as it is */
static int app_int386(int intnum, const union REGS *in, union REGS *out)
{
	PC *pc = app_get_PC();
	CPU *cpu = pc->cpu;
	gprx_t saved[8];
	x86_flags_t saved_flags = cpu->flags;
	uint16_t saved_ss = cpu->ext_accessors->get_seg16(cpu, SEG_SS);
	for (int i = 0; i < 8; i++)
		saved[i] = cpu->gprx[i];
	cpu->gprx[regax].r32 = in->x.eax;
	cpu->gprx[regbx].r32 = in->x.ebx;
	cpu->gprx[regcx].r32 = in->x.ecx;
	cpu->gprx[regdx].r32 = in->x.edx;
	cpu->gprx[regsi].r32 = in->x.esi;
	cpu->gprx[regdi].r32 = in->x.edi;
	app_bios_intcall(cpu, (uint8_t)intnum, "native int386");
	out->x.eax = cpu->gprx[regax].r32;
	out->x.ebx = cpu->gprx[regbx].r32;
	out->x.ecx = cpu->gprx[regcx].r32;
	out->x.edx = cpu->gprx[regdx].r32;
	out->x.esi = cpu->gprx[regsi].r32;
	out->x.edi = cpu->gprx[regdi].r32;
	out->x.cflag = cpu->flags.bits.CF ? 1u : 0u;
	for (int i = 0; i < 8; i++)
		cpu->gprx[i] = saved[i];
	cpu->ext_accessors->set_seg16(cpu, SEG_SS, saved_ss);
	cpu->flags = saved_flags;
	return (int)out->x.eax;
}

static int app_int21_with_ds(uint16_t ds, union REGS *r)
{
	CPU *cpu = app_get_PC()->cpu;
	uint16_t saved_ds = cpu->ext_accessors->get_seg16(cpu, SEG_DS);
	cpu->ext_accessors->set_seg16(cpu, SEG_DS, ds);
	int rc = app_int386(0x21, r, r);
	cpu->ext_accessors->set_seg16(cpu, SEG_DS, saved_ds);
	return rc;
}

/* the runtime's write(): a DOS block as the bounce buffer, then AH=40h */
static int app_write(int fd, const char *s, unsigned n)
{
	union REGS r = { 0 };
	r.h.ah = 0x48;
	r.w.bx = 512 / 16;
	app_int386(0x21, &r, &r);
	frank_printf("hostapp: AH=48h -> ax %04x cf %u\n", r.w.ax, (unsigned)r.x.cflag);
	if (r.x.cflag)
		return -1;
	uint16_t seg = r.w.ax;
	memcpy((uint8_t *)(uintptr_t)FRANK_PSRAM_BASE + ((uint32_t)seg << 4), s, n);
	memset(&r, 0, sizeof r);
	r.h.ah = 0x40;
	r.w.bx = (uint16_t)fd;
	r.w.cx = (uint16_t)n;
	r.w.dx = 0;
	app_int21_with_ds(seg, &r);
	frank_printf("hostapp: AH=40h -> ax %04x cf %u\n", r.w.ax, (unsigned)r.x.cflag);
	return r.x.cflag ? -1 : r.w.ax;
}

int host_native_app(int argc, char **argv)
{
	(void)argv;                             /* (the loader's 32-bit argv_addr: not a host pointer) */
	frank_printf("hostapp: argc %d\n", argc);
	union REGS r = { 0 };
	r.h.ah = 0x30;                          /* DOS version: the simplest result there is */
	app_int386(0x21, &r, &r);
	frank_printf("hostapp: AH=30h -> ax %04x\n", r.w.ax);
	static const char msg[] = "Hello from a native application\r\n";
	app_write(1, msg, sizeof msg - 1);
	return 0;
}
