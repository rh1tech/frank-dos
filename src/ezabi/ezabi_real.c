/*
 * FRANK DOS — the native application ABI's frank-dos side: what the proxy
 * PC/CPU (dos_api.c, murm386's layout) reads from and writes to the real
 * machine, in plain C types (the two sides' CPU and PC types collide).
 *
 * TSR0 (slot 117): murm386 runs it on core 0 at 44.1 kHz, from a repeating
 * timer; its default work (the PIT, the RTC) is frank-dos's pc_step, so
 * the timer starts only when an application installs a callback.
 * TSR1 (slot 118): a core 1 scanline callback in murm386; kept, not called.
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include "pico/time.h"
#include "hle/hle.h"
typedef void (*tsr_callback_t)(void);   /* (apps/api/tsr_callback.h: the applications' side) */

void bios_intcall(CPUI386 *cpu, uint8_t intnum, const char *owner);   /* fdos/dosrt.c */

uint32_t ezabi_real_gpr(int i) { return hle_pc->cpu->gprx[i].r32; }
void ezabi_real_set_gpr(int i, uint32_t v) { hle_pc->cpu->gprx[i].r32 = v; }
uint32_t ezabi_real_flags(void) { return cpu_getflags(hle_pc->cpu); }
void ezabi_real_set_flags(uint32_t v)
{
	uint32_t f = v & 0xFFFFu;
	cpu_setflags(hle_pc->cpu, f, ~f & 0xFFFFu);
}
uint16_t ezabi_real_seg(int s) { return seg_get(hle_pc->cpu, s); }
void ezabi_real_set_seg(int s, uint16_t v) { cpui386_load_seg(hle_pc->cpu, s, v); }
uint16_t ezabi_real_ip(void) { return (uint16_t)hle_pc->cpu->next_ip; }
void ezabi_real_intcall(uint8_t intnum, const char *owner) { bios_intcall(hle_pc->cpu, intnum, owner); }

/* murm386's CPU_CB is frank-dos's first twelve fields */
void ezabi_real_cb(void *cb)
{
	memcpy(cb, &hle_pc->cpu->cb, 12 * sizeof(void *));
}

void ezabi_real_pc_flags(int *pcspk, int *adlib, int *sb16, int *tandy, int *covox, int *mpu401, int *dss)
{
	PC *pc = hle_pc;
	*pcspk = pc->pcspk_enabled;
	*adlib = pc->adlib_enabled;
	*sb16 = pc->sb16_enabled;
	*tandy = pc->tandy_enabled;
	*covox = pc->covox_enabled;
	*mpu401 = pc->mpu401_enabled;
	*dss = pc->dss_enabled;
}

uint8_t *ezabi_video_buffer(uint32_t *size)
{
	if (size)
		*size = (uint32_t)hle_pc->vga_mem_size;
	return (uint8_t *)hle_pc->vga_mem;
}

uint32_t ezabi_psram_size(void) { return hle_ram_size; }

/* ---- TSR0 / TSR1 ---------------------------------------------------------- */
static tsr_callback_t tsr0_callback, tsr1_callback;
static repeating_timer_t tsr0_timer;
static bool tsr0_running;

static bool __not_in_flash_func(tsr0_tick)(repeating_timer_t *rt)
{
	(void)rt;
	tsr_callback_t cb = __atomic_load_n(&tsr0_callback, __ATOMIC_ACQUIRE);
	if (cb)
		cb();
	return true;
}

tsr_callback_t set_tsr0_callback(tsr_callback_t cb)
{
	tsr_callback_t old = __atomic_exchange_n(&tsr0_callback, cb, __ATOMIC_ACQ_REL);
	if (cb && !tsr0_running) {          /* (on core 0: the kernel runs there) */
		tsr0_running = add_repeating_timer_us(-1000000 / 44100, tsr0_tick, NULL, &tsr0_timer);
	}
	return old;
}

tsr_callback_t set_tsr1_callback(tsr_callback_t cb)
{
	return __atomic_exchange_n(&tsr1_callback, cb, __ATOMIC_ACQ_REL);
}
