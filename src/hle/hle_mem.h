/*
 * FRANK DOS — guest memory for native code: plain host memory (the guest's
 * RAM in PSRAM) except the VGA window A0000h-BFFFFh, which goes through the
 * VGA's planes. No register names here: the FreeDOS port includes it.
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: MIT
 */
#ifndef HLE_MEM_H
#define HLE_MEM_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "jit16.h"

#ifdef __cplusplus
extern "C" {
#endif

extern uint8_t *hle_ram;            /* guest physical memory */
extern uint32_t hle_ram_size;

/* ---- guest memory ------------------------------------------------------- */
static inline bool g_is_vga(uint32_t a) { return a - 0xA0000u < 0x20000u; }

uint8_t g_rd8_slow(uint32_t a);
void g_wr8_slow(uint32_t a, uint8_t v);

static inline uint8_t g_rd8(uint32_t a)
{
	return (!g_is_vga(a) && a < hle_ram_size) ? hle_ram[a] : g_rd8_slow(a);
}
static inline void g_wr8(uint32_t a, uint8_t v)
{
	if (!g_is_vga(a) && a < hle_ram_size) {
		hle_ram[a] = v;
		jit_note_write(a, 1);
	} else
		g_wr8_slow(a, v);
}
static inline uint16_t g_rd16(uint32_t a) { return g_rd8(a) | (uint16_t)g_rd8(a + 1) << 8; }
static inline void g_wr16(uint32_t a, uint16_t v) { g_wr8(a, (uint8_t)v); g_wr8(a + 1, v >> 8); }
static inline uint32_t g_rd32(uint32_t a) { return g_rd16(a) | (uint32_t)g_rd16(a + 2) << 16; }
static inline void g_wr32(uint32_t a, uint32_t v) { g_wr16(a, (uint16_t)v); g_wr16(a + 2, v >> 16); }
void g_read(void *dst, uint32_t a, uint32_t n);
void g_write(uint32_t a, const void *src, uint32_t n);

#ifdef __cplusplus
}
#endif

#endif
