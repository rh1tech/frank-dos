/*
 * FRANK DOS — the machine brought up natively: no SeaBIOS, no VGA BIOS.
 *
 * The machine starts from the native BIOS's ROM (hle.c). What is left here
 * from phase 0 is the beep: PIT channel 2 and port 61h, the sound path
 * checked from the console (~C beep~) without any guest code.
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>

#include "pico/stdlib.h"
#include "hle/boot.h"
#include "hle/hle.h"

bool hle_cpu_running;

void hle_outb(PC *pc, int port, uint8_t val)
{
	pc->cpu->cb.io_write8(pc->cpu->cb.io, port, val);
}

uint8_t hle_inb(PC *pc, int port)
{
	return pc->cpu->cb.io_read8(pc->cpu->cb.io, port);
}

/* PIT channel 2 at hz into the speaker for ms */
void hle_beep(PC *pc, unsigned hz, unsigned ms)
{
	unsigned div = 1193182u / hz;
	hle_outb(pc, 0x43, 0xB6);               /* channel 2, lo/hi, square wave */
	hle_outb(pc, 0x42, div & 0xFF);
	hle_outb(pc, 0x42, div >> 8);
	hle_outb(pc, 0x61, hle_inb(pc, 0x61) | 0x03);
	for (unsigned t = 0; t < ms; t += 10) {
		pc_vga_step(pc);
		sleep_ms(10);
	}
	hle_outb(pc, 0x61, hle_inb(pc, 0x61) & ~0x03);
}

void hle_boot(PC *pc)
{
	/* the ROM at F0000h and the trap, then the CPU from F000:FFF0: POST is
	 * the ROM's (hle.c), and from it INT 19h boots a disk */
	hle_bios_install(pc);
	cpui386_reset(pc->cpu);
	hle_cpu_running = true;
	printf("hle_boot: CPU reset to F000:FFF0\n");
}
