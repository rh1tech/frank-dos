/*
 * FRANK DOS — INT 10h, the video BIOS, native.
 *
 * A VGA BIOS done in ARM code on the emulated VGA, through its own ports and
 * memory: the IBM mode table (the register values are DnCraptor's
 * bios_10h.c, which are IBM's), the default DAC palettes, the ROM fonts in
 * plane 2, and the text and graphics services that DOS programs call:
 * cursor, scrolling, characters in text and in CGA / planar / 256-colour
 * modes, pixels, palette, fonts, the display combination.
 *
 * State lives where the IBM BIOS keeps it, in the BIOS data area, so
 * programs that read it there (or set it themselves) see one machine.
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>

#include "hle/hle.h"

/* BDA video fields */
#define B_MODE      BDA(0x49)
#define B_COLS      BDA(0x4A)
#define B_PAGESIZE  BDA(0x4C)
#define B_PAGEOFF   BDA(0x4E)
#define B_CURPOS(p) BDA(0x50 + 2 * (p))
#define B_CURSHAPE  BDA(0x60)
#define B_PAGE      BDA(0x62)
#define B_CRTC      BDA(0x63)
#define B_MODECTL   BDA(0x65)
#define B_PALETTE   BDA(0x66)
#define B_ROWS      BDA(0x84)
#define B_CHARH     BDA(0x85)
#define B_VIDCTL    BDA(0x87)
#define B_SWITCHES  BDA(0x88)
#define B_MODESET   BDA(0x89)
#define B_DCC       BDA(0x8A)

typedef struct {
	uint8_t misc;
	uint8_t seq[5];
	uint8_t crtc[25];
	uint8_t gc[9];
	uint8_t ac[21];
} vregs_t;

enum { M_TEXT, M_CGA4, M_CGA2, M_PLANAR, M_PLANAR2, M_LINEAR };

typedef struct {
	uint8_t mode, kind, cols, rows, charh;
	uint16_t pagesize, crtc;
	uint32_t mem, memsize;
	const vregs_t *r;
} vmode_t;

static const vregs_t r_text80 = {
	0x67, {0x03, 0x00, 0x03, 0x00, 0x02},
	{0x5F, 0x4F, 0x50, 0x82, 0x55, 0x81, 0xBF, 0x1F, 0x00, 0x4F, 0x0D, 0x0E, 0x00, 0x00, 0x00, 0x00,
	 0x9C, 0x0E, 0x8F, 0x28, 0x1F, 0x96, 0xB9, 0xA3, 0xFF},
	{0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x0E, 0x00, 0xFF},
	{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x14, 0x07, 0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F,
	 0x0C, 0x00, 0x0F, 0x08, 0x00}};
static const vregs_t r_text40 = {
	0x67, {0x03, 0x08, 0x03, 0x00, 0x02},
	{0x2D, 0x27, 0x28, 0x90, 0x2B, 0x80, 0xBF, 0x1F, 0x00, 0x4F, 0x0D, 0x0E, 0x00, 0x00, 0x00, 0x00,
	 0x9C, 0x0E, 0x8F, 0x14, 0x1F, 0x96, 0xB9, 0xA3, 0xFF},
	{0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x0E, 0x00, 0xFF},
	{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x14, 0x07, 0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F,
	 0x0C, 0x00, 0x0F, 0x08, 0x00}};
static const vregs_t r_mono = {
	0x66, {0x03, 0x00, 0x03, 0x00, 0x02},
	{0x5F, 0x4F, 0x50, 0x82, 0x55, 0x81, 0xBF, 0x1F, 0x00, 0x4F, 0x0D, 0x0E, 0x00, 0x00, 0x00, 0x00,
	 0x9C, 0x0E, 0x8F, 0x28, 0x0F, 0x96, 0xB9, 0xA3, 0xFF},
	{0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x0A, 0x00, 0xFF},
	{0x00, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x10, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18,
	 0x0E, 0x00, 0x0F, 0x08, 0x00}};
static const vregs_t r_cga4 = {
	0x63, {0x03, 0x09, 0x03, 0x00, 0x02},
	{0x2D, 0x27, 0x28, 0x90, 0x2B, 0x80, 0xBF, 0x1F, 0x00, 0xC1, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	 0x9C, 0x0E, 0x8F, 0x14, 0x00, 0x96, 0xB9, 0xA3, 0xFF},
	{0x00, 0x00, 0x00, 0x00, 0x00, 0x30, 0x0F, 0x00, 0xFF},
	{0x00, 0x13, 0x15, 0x17, 0x02, 0x04, 0x06, 0x07, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
	 0x01, 0x00, 0x03, 0x00, 0x00}};
static const vregs_t r_cga2 = {
	0x63, {0x03, 0x01, 0x01, 0x00, 0x06},
	{0x5F, 0x4F, 0x50, 0x82, 0x54, 0x80, 0xBF, 0x1F, 0x00, 0xC1, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	 0x9C, 0x0E, 0x8F, 0x28, 0x00, 0x96, 0xB9, 0xA3, 0xFF},
	{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0D, 0x00, 0xFF},
	{0x00, 0x17, 0x17, 0x17, 0x17, 0x17, 0x17, 0x17, 0x17, 0x17, 0x17, 0x17, 0x17, 0x17, 0x17, 0x17,
	 0x01, 0x00, 0x01, 0x00, 0x00}};
static const vregs_t r_ega320 = {
	0x63, {0x03, 0x09, 0x0F, 0x00, 0x06},
	{0x2D, 0x27, 0x28, 0x90, 0x2B, 0x80, 0xBF, 0x1F, 0x00, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	 0x9C, 0x8E, 0x8F, 0x14, 0x00, 0x96, 0xB9, 0xE3, 0xFF},
	{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05, 0x0F, 0xFF},
	{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
	 0x01, 0x00, 0x0F, 0x00, 0x00}};
static const vregs_t r_ega640 = {
	0x63, {0x03, 0x01, 0x0F, 0x00, 0x06},
	{0x5F, 0x4F, 0x50, 0x82, 0x54, 0x80, 0xBF, 0x1F, 0x00, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	 0x9C, 0x8E, 0x8F, 0x28, 0x00, 0x96, 0xB9, 0xE3, 0xFF},
	{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05, 0x0F, 0xFF},
	{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
	 0x01, 0x00, 0x0F, 0x00, 0x00}};
static const vregs_t r_ega350 = {
	0xA3, {0x03, 0x01, 0x0F, 0x00, 0x06},
	{0x5F, 0x4F, 0x50, 0x82, 0x54, 0x80, 0xBF, 0x1F, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	 0x83, 0x85, 0x5D, 0x28, 0x0F, 0x63, 0xBA, 0xE3, 0xFF},
	{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05, 0x0F, 0xFF},
	{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x14, 0x07, 0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F,
	 0x01, 0x00, 0x0F, 0x00, 0x00}};
static const vregs_t r_vga2 = {
	0xE3, {0x03, 0x01, 0x0F, 0x00, 0x06},
	{0x5F, 0x4F, 0x50, 0x82, 0x54, 0x80, 0x0B, 0x3E, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	 0xEA, 0x8C, 0xDF, 0x28, 0x00, 0xE7, 0x04, 0xC3, 0xFF},
	{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05, 0x0F, 0xFF},
	{0x00, 0x3F, 0x3F, 0x3F, 0x3F, 0x3F, 0x3F, 0x3F, 0x3F, 0x3F, 0x3F, 0x3F, 0x3F, 0x3F, 0x3F, 0x3F,
	 0x01, 0x00, 0x0F, 0x00, 0x00}};
static const vregs_t r_vga16 = {
	0xE3, {0x03, 0x01, 0x0F, 0x00, 0x06},
	{0x5F, 0x4F, 0x50, 0x82, 0x54, 0x80, 0x0B, 0x3E, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	 0xEA, 0x8C, 0xDF, 0x28, 0x00, 0xE7, 0x04, 0xE3, 0xFF},
	{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05, 0x0F, 0xFF},
	{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x14, 0x07, 0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F,
	 0x01, 0x00, 0x0F, 0x00, 0x00}};
static const vregs_t r_vga256 = {
	0x63, {0x03, 0x01, 0x0F, 0x00, 0x0E},
	{0x5F, 0x4F, 0x50, 0x82, 0x54, 0x80, 0xBF, 0x1F, 0x00, 0x41, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	 0x9C, 0x0E, 0x8F, 0x28, 0x40, 0x96, 0xB9, 0xA3, 0xFF},
	{0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x05, 0x0F, 0xFF},
	{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
	 0x41, 0x00, 0x0F, 0x00, 0x00}};

static const vmode_t modes[] = {
	{0x00, M_TEXT,    40, 25, 16, 0x0800, 0x3D4, 0xB8000, 0x8000, &r_text40},
	{0x01, M_TEXT,    40, 25, 16, 0x0800, 0x3D4, 0xB8000, 0x8000, &r_text40},
	{0x02, M_TEXT,    80, 25, 16, 0x1000, 0x3D4, 0xB8000, 0x8000, &r_text80},
	{0x03, M_TEXT,    80, 25, 16, 0x1000, 0x3D4, 0xB8000, 0x8000, &r_text80},
	{0x04, M_CGA4,    40, 25,  8, 0x4000, 0x3D4, 0xB8000, 0x4000, &r_cga4},
	{0x05, M_CGA4,    40, 25,  8, 0x4000, 0x3D4, 0xB8000, 0x4000, &r_cga4},
	{0x06, M_CGA2,    80, 25,  8, 0x4000, 0x3D4, 0xB8000, 0x4000, &r_cga2},
	{0x07, M_TEXT,    80, 25, 16, 0x1000, 0x3B4, 0xB0000, 0x8000, &r_mono},
	{0x0D, M_PLANAR,  40, 25,  8, 0x2000, 0x3D4, 0xA0000, 0x10000, &r_ega320},
	{0x0E, M_PLANAR,  80, 25,  8, 0x4000, 0x3D4, 0xA0000, 0x10000, &r_ega640},
	{0x0F, M_PLANAR2, 80, 25, 14, 0x8000, 0x3B4, 0xA0000, 0x10000, &r_ega350},
	{0x10, M_PLANAR,  80, 25, 14, 0x8000, 0x3D4, 0xA0000, 0x10000, &r_ega350},
	{0x11, M_PLANAR2, 80, 30, 16, 0x0000, 0x3D4, 0xA0000, 0x10000, &r_vga2},
	{0x12, M_PLANAR,  80, 30, 16, 0x0000, 0x3D4, 0xA0000, 0x10000, &r_vga16},
	{0x13, M_LINEAR,  40, 25,  8, 0x0000, 0x3D4, 0xA0000, 0x10000, &r_vga256},
};

static const vmode_t *cur;          /* the mode set last (by us) */

static const vmode_t *find_mode(uint8_t m)
{
	for (unsigned i = 0; i < sizeof modes / sizeof modes[0]; i++)
		if (modes[i].mode == m)
			return &modes[i];
	return NULL;
}

/* ---- registers ------------------------------------------------------------ */
static uint16_t crtc_port(void) { uint16_t p = g_rd16(B_CRTC); return p ? p : 0x3D4; }
static void seq(int i, uint8_t v) { outb(0x3C4, i); outb(0x3C5, v); }
static void gc(int i, uint8_t v)  { outb(0x3CE, i); outb(0x3CF, v); }
static uint8_t gc_get(int i)      { outb(0x3CE, i); return inb(0x3CF); }
static void crtc(int i, uint8_t v) { uint16_t p = crtc_port(); outb(p, i); outb(p + 1, v); }
static uint8_t crtc_get(int i)    { uint16_t p = crtc_port(); outb(p, i); return inb(p + 1); }

static void ac_write(int i, uint8_t v)
{
	(void)inb(crtc_port() + 6);
	outb(0x3C0, i);
	outb(0x3C0, v);
	outb(0x3C0, 0x20);
}

static uint8_t ac_read(int i)
{
	(void)inb(crtc_port() + 6);
	outb(0x3C0, i);
	uint8_t v = inb(0x3C1);
	(void)inb(crtc_port() + 6);
	outb(0x3C0, 0x20);
	return v;
}

static void program(const vmode_t *m)
{
	const vregs_t *r = m->r;
	outb(0x3C2, r->misc);
	for (int i = 0; i < 5; i++)
		seq(i, r->seq[i]);
	outb(m->crtc, 0x11);
	outb(m->crtc + 1, r->crtc[0x11] & 0x7F);
	for (int i = 0; i < 25; i++) {
		outb(m->crtc, i);
		outb(m->crtc + 1, r->crtc[i]);
	}
	for (int i = 0; i < 9; i++)
		gc(i, r->gc[i]);
	(void)inb(m->crtc + 6);
	for (int i = 0; i < 21; i++) {
		outb(0x3C0, i);
		outb(0x3C0, r->ac[i]);
	}
	outb(0x3C0, 0x20);
}

/* ---- the DAC ---------------------------------------------------------------- */
static void dac_set(int i, uint8_t r, uint8_t g, uint8_t b)
{
	outb(0x3C8, i);
	outb(0x3C9, r);
	outb(0x3C9, g);
	outb(0x3C9, b);
}

/* the 64 EGA colours (rgbRGB) in 0-63: the 16-colour and text modes */
static void dac_ega(void)
{
	outb(0x3C8, 0);
	for (int i = 0; i < 64; i++) {
		outb(0x3C9, 0x2A * ((i >> 2) & 1) + 0x15 * ((i >> 5) & 1));
		outb(0x3C9, 0x2A * ((i >> 1) & 1) + 0x15 * ((i >> 4) & 1));
		outb(0x3C9, 0x2A * (i & 1) + 0x15 * ((i >> 3) & 1));
	}
}

/* the 200-line modes (4-6, 0Dh, 0Eh): CGA colours, bit 4 is the intensity
   (their attributes 8-15 are 10h-17h); bits 3 and 5 don't count, 6 is brown */
static void dac_cga(void)
{
	outb(0x3C8, 0);
	for (int i = 0; i < 64; i++) {
		int hi = 0x15 * ((i >> 4) & 1);
		outb(0x3C9, 0x2A * ((i >> 2) & 1) + hi);
		outb(0x3C9, (i & 0x17) == 6 ? 0x15 : 0x2A * ((i >> 1) & 1) + hi);
		outb(0x3C9, 0x2A * (i & 1) + hi);
	}
}

/* the IBM default 256: 16 CGA colours, 16 greys, 9 hue wheels, 8 black */
static void dac_vga256(void)
{
	static const uint8_t grey[16] = {0, 5, 8, 11, 14, 17, 20, 24, 28, 32, 36, 40, 45, 50, 56, 63};
	static const uint8_t lv[9][5] = {
		{0x00, 0x10, 0x1F, 0x2F, 0x3F}, {0x1F, 0x27, 0x2F, 0x37, 0x3F}, {0x2D, 0x31, 0x36, 0x3A, 0x3F},
		{0x00, 0x07, 0x0E, 0x15, 0x1C}, {0x0E, 0x11, 0x15, 0x18, 0x1C}, {0x14, 0x16, 0x18, 0x1A, 0x1C},
		{0x00, 0x04, 0x08, 0x0C, 0x10}, {0x08, 0x0A, 0x0C, 0x0E, 0x10}, {0x0B, 0x0C, 0x0D, 0x0F, 0x10}};
	static const uint8_t hue[24][3] = {
		{0, 0, 4}, {1, 0, 4}, {2, 0, 4}, {3, 0, 4}, {4, 0, 4}, {4, 0, 3}, {4, 0, 2}, {4, 0, 1},
		{4, 0, 0}, {4, 1, 0}, {4, 2, 0}, {4, 3, 0}, {4, 4, 0}, {3, 4, 0}, {2, 4, 0}, {1, 4, 0},
		{0, 4, 0}, {0, 4, 1}, {0, 4, 2}, {0, 4, 3}, {0, 4, 4}, {0, 3, 4}, {0, 2, 4}, {0, 1, 4}};
	outb(0x3C8, 0);
	for (int i = 0; i < 16; i++) {          /* CGA: rgbI, brown */
		uint8_t r = (i & 4) ? 0x2A : 0, g = (i & 2) ? 0x2A : 0, b = (i & 1) ? 0x2A : 0;
		if (i == 6)
			g = 0x15;
		if (i & 8) {
			r += 0x15;
			g += 0x15;
			b += 0x15;
		}
		outb(0x3C9, r); outb(0x3C9, g); outb(0x3C9, b);
	}
	for (int i = 0; i < 16; i++) {
		outb(0x3C9, grey[i]); outb(0x3C9, grey[i]); outb(0x3C9, grey[i]);
	}
	for (int w = 0; w < 9; w++)
		for (int h = 0; h < 24; h++)
			for (int c = 0; c < 3; c++)
				outb(0x3C9, lv[w][hue[h][c]]);
	for (int i = 248; i < 256; i++) {
		outb(0x3C9, 0); outb(0x3C9, 0); outb(0x3C9, 0);
	}
}

/* ---- fonts ------------------------------------------------------------------- */
static uint32_t rom_font(int h)
{
	return ROM_BASE + (h == 8 ? ROM_FONT8X8 : h == 14 ? ROM_FONT8X14 : ROM_FONT8X16);
}

/* glyphs into plane 2, block blk (8 KB apart), h bytes each, 32-byte cells */
static void font_load(uint32_t src, int blk, int first, int count, int h)
{
	seq(2, 0x04);
	seq(4, 0x07);
	gc(4, 0x02);
	gc(5, 0x00);
	gc(6, 0x04);
	uint32_t base = (uint32_t)(blk & 3) * 0x4000 + (blk & 4 ? 0x2000 : 0);
	for (int c = first; c < first + count && c < 256; c++)
		for (int r = 0; r < 32; r++)
			vga_mem_write(hle_pc->vga, base + c * 32 + r, r < h ? g_rd8(src + (c - first) * h + r) : 0);
	seq(2, 0x03);
	seq(4, 0x02);
	gc(4, 0x00);
	gc(5, 0x10);
	gc(6, g_rd8(B_MODE) == 7 ? 0x0A : 0x0E);
}

/* the text geometry for a character height (AH=11h's 1xh functions) */
static void text_geometry(int h)
{
	int lines = 400;                        /* the 400-line text modes */
	crtc(0x09, (crtc_get(0x09) & 0xE0) | (h - 1));
	crtc(0x0A, h - 3 < 0 ? 0 : h - 3);
	crtc(0x0B, h - 2 < 0 ? 0 : h - 2);
	int rows = lines / h;
	crtc(0x12, (uint8_t)(rows * h - 1));
	g_wr8(B_ROWS, rows - 1);
	g_wr16(B_CHARH, h);
	g_wr16(B_PAGESIZE, (uint16_t)((rows * g_rd16(B_COLS) * 2 + 0xFF) & ~0xFF));
	g_wr16(B_CURSHAPE, (uint16_t)((h - 3) << 8 | (h - 2)));
}

/* ---- the cursor ------------------------------------------------------------------- */
static int cols(void) { return g_rd16(B_COLS); }
static int rows(void) { return g_rd8(B_ROWS) + 1; }

static void cursor_hw(void)
{
	int page = g_rd8(B_PAGE);
	uint16_t pos = g_rd16(B_CURPOS(page));
	unsigned addr = g_rd16(B_PAGEOFF) / 2 + (pos >> 8) * cols() + (pos & 0xFF);
	crtc(0x0E, addr >> 8);
	crtc(0x0F, addr & 0xFF);
}

static void cursor_shape(uint16_t shape)
{
	g_wr16(B_CURSHAPE, shape);
	uint8_t start = shape >> 8, end = shape & 0x1F;
	int h = g_rd16(B_CHARH);
	if ((start & 0x60) == 0x20) {           /* invisible */
		crtc(0x0A, 0x20);
		return;
	}
	start &= 0x1F;
	/* CGA cursor values (0-7) in a taller cell, as the VGA BIOS emulates them */
	if (h > 8 && start < 8 && end < 8 && !(g_rd8(B_VIDCTL) & 0x01)) {
		if (end != start && end < h - 1) {
			start = (uint8_t)(start * h / 8);
			end = (uint8_t)(end * h / 8 + (h > 14 ? 1 : 0));
		} else if (end == start || end >= 7) {
			start = (uint8_t)(start * h / 8);
			end = (uint8_t)(h - 1);
		}
	}
	crtc(0x0A, start);
	crtc(0x0B, end);
}

/* ---- mode set ---------------------------------------------------------------------- */
static void set_mode(uint8_t al)
{
	uint8_t m = al & 0x7F;
	const vmode_t *v = find_mode(m);
	if (!v)
		return;
	cur = v;
	program(v);
	if (v->kind == M_LINEAR)
		dac_vga256();
	else if (v->kind == M_CGA4 || v->kind == M_CGA2 || (v->kind == M_PLANAR && v->charh == 8))
		dac_cga();
	else
		dac_ega();

	g_wr8(B_MODE, m);
	g_wr16(B_COLS, v->cols);
	g_wr16(B_PAGESIZE, v->pagesize);
	g_wr16(B_PAGEOFF, 0);
	for (int p = 0; p < 8; p++)
		g_wr16(B_CURPOS(p), 0);
	g_wr8(B_PAGE, 0);
	g_wr16(B_CRTC, v->crtc);
	static const uint8_t msr[8] = {0x2C, 0x28, 0x2D, 0x29, 0x2A, 0x2E, 0x1E, 0x29};
	g_wr8(B_MODECTL, m < 8 ? msr[m] : 0x29);
	g_wr8(B_PALETTE, m == 6 ? 0x3F : 0x30);
	g_wr8(B_ROWS, v->rows - 1);
	g_wr16(B_CHARH, v->charh);
	g_wr8(B_VIDCTL, 0x60 | (al & 0x80));
	g_wr8(B_SWITCHES, 0xF9);
	g_wr8(B_MODESET, 0x51);
	g_wr8(B_DCC, 0x08);

	if (v->kind == M_TEXT) {
		font_load(rom_font(16), 0, 0, 256, 16);
		g_wr16(B_CURSHAPE, 0x0607);
		cursor_shape(0x0607);
	}
	/* INT 43h: the graphics font for this height */
	uint16_t f = v->charh == 8 ? ROM_FONT8X8 : v->charh == 14 ? ROM_FONT8X14 : ROM_FONT8X16;
	g_wr16(0x43 * 4, f);
	g_wr16(0x43 * 4 + 2, ROM_SEG);

	if (!(al & 0x80)) {
		if (v->kind == M_TEXT) {
			for (uint32_t a = 0; a < v->memsize; a += 2) {
				g_wr8(v->mem + a, ' ');
				g_wr8(v->mem + a + 1, 0x07);
			}
		} else if (v->kind == M_PLANAR || v->kind == M_PLANAR2 || v->kind == M_LINEAR) {
			seq(2, 0x0F);                   /* every plane, all of video memory */
			for (uint32_t a = 0; a < 0x10000; a++)
				g_wr8(0xA0000 + a, 0);
			seq(2, v->r->seq[2]);
		} else {
			for (uint32_t a = 0; a < v->memsize; a++)
				g_wr8(v->mem + a, 0);
		}
	}
	cursor_hw();
}

/* ---- characters --------------------------------------------------------------------- */
static uint32_t text_addr(int page, int row, int col)
{
	return cur->mem + (uint32_t)page * g_rd16(B_PAGESIZE) + (row * cols() + col) * 2;
}

/* a glyph cell in a graphics mode: fg where its bits are set, bg (0) elsewhere;
 * xor (fg bit 7) leaves the background and xors the foreground */
static void gfx_char(int row, int col, uint8_t ch, uint8_t fg)
{
	int h = g_rd16(B_CHARH);
	uint32_t font = (uint32_t)g_rd16(0x43 * 4 + 2) << 4;
	font += g_rd16(0x43 * 4);
	if (h == 8 && ch >= 128 && cur->kind != M_LINEAR && cur->kind != M_PLANAR && cur->kind != M_PLANAR2) {
		font = ((uint32_t)g_rd16(0x1F * 4 + 2) << 4) + g_rd16(0x1F * 4);    /* CGA: INT 1Fh */
		ch -= 128;
	}
	bool x = fg & 0x80;
	const uint32_t gl = font + (uint32_t)ch * h;
	switch (cur->kind) {
	case M_LINEAR:
		for (int r = 0; r < h; r++) {
			uint8_t bits = g_rd8(gl + r);
			uint32_t a = 0xA0000 + (row * h + r) * 320 + col * 8;
			for (int i = 0; i < 8; i++) {
				bool on = bits & (0x80 >> i);
				if (x)
					g_wr8(a + i, g_rd8(a + i) ^ (on ? (fg & 0x7F) : 0));
				else
					g_wr8(a + i, on ? fg : 0);
			}
		}
		break;
	case M_CGA4:
		for (int r = 0; r < h; r++) {
			uint8_t bits = g_rd8(gl + r);
			int y = row * h + r;
			uint32_t a = 0xB8000 + (y & 1) * 0x2000 + (y >> 1) * 80 + col * 2;
			uint16_t px = 0;
			for (int i = 0; i < 8; i++)
				if (bits & (0x80 >> i))
					px |= (uint16_t)(fg & 3) << (14 - 2 * i);
			uint8_t hi = px >> 8, lo = px & 0xFF;
			if (x) {
				hi ^= g_rd8(a);
				lo ^= g_rd8(a + 1);
			}
			g_wr8(a, hi);
			g_wr8(a + 1, lo);
		}
		break;
	case M_CGA2:
		for (int r = 0; r < h; r++) {
			uint8_t bits = (fg & 1) ? g_rd8(gl + r) : 0;
			int y = row * h + r;
			uint32_t a = 0xB8000 + (y & 1) * 0x2000 + (y >> 1) * 80 + col;
			g_wr8(a, x ? (uint8_t)(g_rd8(a) ^ bits) : bits);
		}
		break;
	default: {                              /* planar: write mode 2 through the bit mask */
		int bpl = cols();
		uint32_t pg = (uint32_t)g_rd8(B_PAGE) * g_rd16(B_PAGESIZE);
		seq(2, 0x0F);
		gc(5, 0x02);
		gc(3, x ? 0x18 : 0x00);
		for (int r = 0; r < h; r++) {
			uint8_t bits = g_rd8(gl + r);
			uint32_t a = 0xA0000 + pg + (row * h + r) * bpl + col;
			if (!x) {
				gc(8, 0xFF);
				(void)g_rd8(a);
				g_wr8(a, 0);
			}
			gc(8, bits);
			(void)g_rd8(a);
			g_wr8(a, fg & 0x0F);
		}
		gc(8, 0xFF);
		gc(3, 0x00);
		gc(5, 0x00);
		break;
	}
	}
}

static void write_char(int page, uint8_t ch, uint8_t attr, int count, bool with_attr)
{
	uint16_t pos = g_rd16(B_CURPOS(page));
	int row = pos >> 8, col = pos & 0xFF;
	while (count-- > 0) {
		if (cur->kind == M_TEXT) {
			uint32_t a = text_addr(page, row, col);
			g_wr8(a, ch);
			if (with_attr)
				g_wr8(a + 1, attr);
		} else
			gfx_char(row, col, ch, attr);
		if (++col >= cols()) {
			col = 0;
			if (++row >= rows())
				break;
		}
	}
}

/* AH=06h/07h: lines up (or down) in the window; 0 lines clears it */
static void scroll(int lines, uint8_t attr, int top, int left, int bot, int right, bool up, int page)
{
	if (right >= cols())
		right = cols() - 1;
	if (bot >= rows())
		bot = rows() - 1;
	if (top > bot || left > right)
		return;
	int height = bot - top + 1;
	if (lines == 0 || lines > height)
		lines = height;
	if (cur->kind == M_TEXT) {
		for (int i = 0; i < height; i++) {
			int dst = up ? top + i : bot - i;
			int src = up ? dst + lines : dst - lines;
			bool blank = up ? src > bot : src < top;
			for (int c = left; c <= right; c++) {
				uint32_t d = text_addr(page, dst, c);
				if (blank) {
					g_wr8(d, ' ');
					g_wr8(d + 1, attr);
				} else {
					uint32_t s = text_addr(page, src, c);
					g_wr8(d, g_rd8(s));
					g_wr8(d + 1, g_rd8(s + 1));
				}
			}
		}
		return;
	}
	/* graphics: pixel rows of character cells */
	int h = g_rd16(B_CHARH);
	for (int i = 0; i < height; i++) {
		int dst = up ? top + i : bot - i;
		int src = up ? dst + lines : dst - lines;
		bool blank = up ? src > bot : src < top;
		for (int r = 0; r < h; r++) {
			int dy = dst * h + r, sy = src * h + r;
			switch (cur->kind) {
			case M_LINEAR:
				for (int x = left * 8; x < (right + 1) * 8; x++)
					g_wr8(0xA0000 + dy * 320 + x, blank ? attr : g_rd8(0xA0000 + sy * 320 + x));
				break;
			case M_CGA4:
			case M_CGA2: {
				int bpc = cur->kind == M_CGA4 ? 2 : 1;
				uint32_t d = 0xB8000 + (dy & 1) * 0x2000 + (dy >> 1) * 80;
				uint32_t s = 0xB8000 + (sy & 1) * 0x2000 + (sy >> 1) * 80;
				uint8_t fill = cur->kind == M_CGA4 ? (uint8_t)((attr & 3) * 0x55) : (attr & 1 ? 0xFF : 0);
				for (int x = left * bpc; x < (right + 1) * bpc; x++)
					g_wr8(d + x, blank ? fill : g_rd8(s + x));
				break;
			}
			default: {                  /* planar: write mode 1 copies the latches */
				int bpl = cols();
				seq(2, 0x0F);
				if (blank) {
					gc(5, 0x02);
					gc(8, 0xFF);
					for (int x = left; x <= right; x++) {
						(void)g_rd8(0xA0000 + dy * bpl + x);
						g_wr8(0xA0000 + dy * bpl + x, attr & 0x0F);
					}
				} else {
					gc(5, 0x01);
					for (int x = left; x <= right; x++) {
						(void)g_rd8(0xA0000 + sy * bpl + x);
						g_wr8(0xA0000 + dy * bpl + x, 0);
					}
				}
				gc(5, 0x00);
				break;
			}
			}
		}
	}
}

static void tty_attr(uint8_t ch, int page, uint8_t attr, bool set_attr)
{
	uint16_t pos = g_rd16(B_CURPOS(page));
	int row = pos >> 8, col = pos & 0xFF;
	switch (ch) {
	case 7:
		return;                             /* the bell: silent */
	case 8:
		if (col > 0)
			col--;
		break;
	case 10:
		row++;
		break;
	case 13:
		col = 0;
		break;
	default:
		if (cur->kind == M_TEXT) {
			g_wr8(text_addr(page, row, col), ch);
			if (set_attr)
				g_wr8(text_addr(page, row, col) + 1, attr);
		} else
			gfx_char(row, col, ch, attr);
		col++;
		break;
	}
	if (col >= cols()) {
		col = 0;
		row++;
	}
	if (row >= rows()) {
		uint8_t fill = 0;
		if (cur->kind == M_TEXT)
			fill = g_rd8(text_addr(page, rows() - 1, cols() - 1) + 1);
		scroll(1, fill, 0, 0, rows() - 1, cols() - 1, true, page);
		row = rows() - 1;
	}
	g_wr16(B_CURPOS(page), (uint16_t)(row << 8 | col));
	if (page == g_rd8(B_PAGE))
		cursor_hw();
}

static void tty(uint8_t ch, int page, uint8_t attr) { tty_attr(ch, page, attr, false); }

void int10_tty(uint8_t ch) { tty(ch, g_rd8(B_PAGE), 0x07); }
void int10_puts(const char *s) { while (*s) int10_tty((uint8_t)*s++); }

/* ---- pixels ---------------------------------------------------------------------------- */
static void put_pixel(int x, int y, uint8_t c, int page)
{
	switch (cur->kind) {
	case M_LINEAR: {
		uint32_t a = 0xA0000 + y * 320 + x;
		g_wr8(a, (c & 0x80) ? (uint8_t)(g_rd8(a) ^ (c & 0x7F)) : c);
		break;
	}
	case M_CGA4:
	case M_CGA2: {
		bool two = cur->kind == M_CGA4;
		uint32_t a = 0xB8000 + (y & 1) * 0x2000 + (y >> 1) * 80 + (two ? x >> 2 : x >> 3);
		int sh = two ? (3 - (x & 3)) * 2 : 7 - (x & 7);
		uint8_t m = (uint8_t)((two ? 3 : 1) << sh), v = (uint8_t)((c & (two ? 3 : 1)) << sh);
		uint8_t old = g_rd8(a);
		g_wr8(a, (c & 0x80) ? (uint8_t)(old ^ v) : (uint8_t)((old & ~m) | v));
		break;
	}
	case M_PLANAR:
	case M_PLANAR2: {
		uint32_t a = 0xA0000 + (uint32_t)page * g_rd16(B_PAGESIZE) + y * cols() + (x >> 3);
		seq(2, 0x0F);
		gc(5, 0x02);
		gc(3, (c & 0x80) ? 0x18 : 0x00);
		gc(8, 0x80 >> (x & 7));
		(void)g_rd8(a);
		g_wr8(a, c & 0x0F);
		gc(8, 0xFF);
		gc(3, 0x00);
		gc(5, 0x00);
		break;
	}
	default:
		break;
	}
}

static uint8_t get_pixel(int x, int y, int page)
{
	switch (cur->kind) {
	case M_LINEAR:
		return g_rd8(0xA0000 + y * 320 + x);
	case M_CGA4:
	case M_CGA2: {
		bool two = cur->kind == M_CGA4;
		uint8_t b = g_rd8(0xB8000 + (y & 1) * 0x2000 + (y >> 1) * 80 + (two ? x >> 2 : x >> 3));
		return two ? (b >> ((3 - (x & 3)) * 2)) & 3 : (b >> (7 - (x & 7))) & 1;
	}
	case M_PLANAR:
	case M_PLANAR2: {
		uint32_t a = 0xA0000 + (uint32_t)page * g_rd16(B_PAGESIZE) + y * cols() + (x >> 3);
		uint8_t v = 0;
		for (int p = 0; p < 4; p++) {
			gc(4, p);
			v |= ((g_rd8(a) >> (7 - (x & 7))) & 1) << p;
		}
		gc(4, 0);
		return v;
	}
	default:
		return 0;
	}
}

/* ---- AH=10h, the palette --------------------------------------------------------------- */
static void palette(CPUI386 *cpu)
{
	switch (AL) {
	case 0x00: ac_write(BL & 0x1F, BH); break;
	case 0x01: ac_write(0x11, BH); break;
	case 0x02: {
		uint32_t p = LIN(ES, DX);
		for (int i = 0; i < 16; i++)
			ac_write(i, g_rd8(p + i));
		ac_write(0x11, g_rd8(p + 16));
		break;
	}
	case 0x03:                              /* blink (1) or intensity (0) */
		ac_write(0x10, (ac_read(0x10) & ~0x08) | (BL ? 0x08 : 0));
		break;
	case 0x07: BH = ac_read(BL & 0x1F); break;
	case 0x08: BH = ac_read(0x11); break;
	case 0x09: {
		uint32_t p = LIN(ES, DX);
		for (int i = 0; i < 16; i++)
			g_wr8(p + i, ac_read(i));
		g_wr8(p + 16, ac_read(0x11));
		break;
	}
	case 0x10: dac_set(BX & 0xFF, DH, CH, CL); break;
	case 0x12: {
		uint32_t p = LIN(ES, DX);
		outb(0x3C8, BX & 0xFF);
		for (int i = 0; i < CX * 3; i++)
			outb(0x3C9, g_rd8(p + i));
		break;
	}
	case 0x13:
		if (BL == 0)
			ac_write(0x10, (ac_read(0x10) & ~0x80) | (BH & 1 ? 0x80 : 0));
		else
			ac_write(0x14, BH);
		break;
	case 0x15:
		outb(0x3C7, BX & 0xFF);
		DH = inb(0x3C9);
		CH = inb(0x3C9);
		CL = inb(0x3C9);
		break;
	case 0x17: {
		uint32_t p = LIN(ES, DX);
		outb(0x3C7, BX & 0xFF);
		for (int i = 0; i < CX * 3; i++)
			g_wr8(p + i, inb(0x3C9));
		break;
	}
	case 0x18: outb(0x3C6, BL); break;
	case 0x19: BL = inb(0x3C6); break;
	case 0x1A: {
		uint8_t m = ac_read(0x10);
		BL = (m >> 7) & 1;
		BH = (m & 0x80) ? (ac_read(0x14) & 0x0F) : ((ac_read(0x14) >> 2) & 0x03);
		break;
	}
	case 0x1B: {                            /* sum to grey */
		for (int i = 0; i < CX; i++) {
			int n = (BX + i) & 0xFF;
			outb(0x3C7, n);
			int r = inb(0x3C9), g = inb(0x3C9), b = inb(0x3C9);
			int y = (r * 30 + g * 59 + b * 11 + 50) / 100;
			dac_set(n, y, y, y);
		}
		break;
	}
	default:
		break;
	}
}

/* ---- AH=11h, fonts ------------------------------------------------------------------------ */
static void fonts(CPUI386 *cpu)
{
	switch (AL) {
	case 0x00: case 0x10:                   /* user font, ES:BP, BH bytes a glyph */
		font_load(LIN(ES, BP), BL, DX, CX, BH);
		if (AL == 0x10)
			text_geometry(BH);
		break;
	case 0x01: case 0x11: font_load(rom_font(14), BL, 0, 256, 14); if (AL == 0x11) text_geometry(14); break;
	case 0x02: case 0x12: font_load(rom_font(8), BL, 0, 256, 8); if (AL == 0x12) text_geometry(8); break;
	case 0x04: case 0x14: font_load(rom_font(16), BL, 0, 256, 16); if (AL == 0x14) text_geometry(16); break;
	case 0x03: seq(3, BL); break;
	case 0x20: g_wr16(0x1F * 4, BP); g_wr16(0x1F * 4 + 2, ES); break;
	case 0x21: case 0x22: case 0x23: case 0x24: {   /* the graphics font, INT 43h */
		int h = AL == 0x21 ? CX : AL == 0x22 ? 14 : AL == 0x23 ? 8 : 16;
		if (AL == 0x21) {
			g_wr16(0x43 * 4, BP);
			g_wr16(0x43 * 4 + 2, ES);
		} else {
			g_wr16(0x43 * 4, (uint16_t)(rom_font(h) - ROM_BASE));
			g_wr16(0x43 * 4 + 2, ROM_SEG);
		}
		g_wr16(B_CHARH, h);
		int r = BL == 0 ? DL : BL == 1 ? 14 : BL == 2 ? 25 : 43;
		g_wr8(B_ROWS, r - 1);
		break;
	}
	case 0x30: {                            /* font information */
		uint16_t off = 0, sg = ROM_SEG;
		switch (BH) {
		case 0: off = g_rd16(0x1F * 4); sg = g_rd16(0x1F * 4 + 2); break;
		case 1: off = g_rd16(0x43 * 4); sg = g_rd16(0x43 * 4 + 2); break;
		case 2: off = ROM_FONT8X14; break;
		case 3: off = ROM_FONT8X8; break;
		case 4: off = ROM_FONT8X8_HI; break;
		case 5: off = ROM_FONT8X14; break;
		case 6: off = ROM_FONT8X16; break;
		case 7: off = ROM_FONT8X16; break;
		}
		seg_set(cpu, HS_ES, sg);
		BP = off;
		CX = g_rd16(B_CHARH);
		DL = g_rd8(B_ROWS);
		break;
	}
	default:
		break;
	}
}

/* ---- INT 10h --------------------------------------------------------------------------------- */
enum hle_result hle_int10(CPUI386 *cpu)
{
	if (!cur)
		cur = find_mode(3);
	switch (AH) {
	case 0x00:
		set_mode(AL);
		AL = AL > 7 ? 0x20 : (AL == 6 ? 0x3F : 0x30);
		break;
	case 0x01:
		cursor_shape(CX);
		break;
	case 0x02:
		g_wr16(B_CURPOS(BH & 7), DX);
		if ((BH & 7) == g_rd8(B_PAGE))
			cursor_hw();
		break;
	case 0x03:
		DX = g_rd16(B_CURPOS(BH & 7));
		CX = g_rd16(B_CURSHAPE);
		break;
	case 0x04:                              /* light pen: none */
		AH = 0;
		break;
	case 0x05: {
		int p = AL & 7;
		g_wr8(B_PAGE, p);
		uint16_t off = (uint16_t)(p * g_rd16(B_PAGESIZE));
		g_wr16(B_PAGEOFF, off);
		unsigned start = cur->kind == M_TEXT ? off / 2 : off;
		crtc(0x0C, start >> 8);
		crtc(0x0D, start & 0xFF);
		cursor_hw();
		break;
	}
	case 0x06:
	case 0x07:
		scroll(AL, BH, CH, CL, DH, DL, AH == 0x06, g_rd8(B_PAGE));
		break;
	case 0x08:
		if (cur->kind == M_TEXT) {
			uint16_t pos = g_rd16(B_CURPOS(BH & 7));
			uint32_t a = text_addr(BH & 7, pos >> 8, pos & 0xFF);
			AL = g_rd8(a);
			AH = g_rd8(a + 1);
		} else
			AX = 0;
		break;
	case 0x09:
		write_char(BH & 7, AL, BL, CX, true);
		break;
	case 0x0A:
		write_char(BH & 7, AL, BL, CX, false);
		break;
	case 0x0B:                              /* CGA palette, border */
		if (BH == 0) {
			ac_write(0x11, BL & 0x1F);
			if (cur->kind == M_CGA4 || cur->kind == M_CGA2)
				ac_write(0x00, BL & 0x0F);
		} else if (cur->kind == M_CGA4) {
			int p = BL & 1;
			for (int i = 1; i < 4; i++)
				ac_write(i, (uint8_t)(i * 2 + p + 0x10));
		}
		break;
	case 0x0C:
		put_pixel(CX, DX, AL, BH & 7);
		break;
	case 0x0D:
		AL = get_pixel(CX, DX, BH & 7);
		break;
	case 0x0E:
		tty(AL, g_rd8(B_PAGE), BL);
		break;
	case 0x0F:
		AL = g_rd8(B_MODE) | (g_rd8(B_VIDCTL) & 0x80);
		AH = (uint8_t)cols();
		BH = g_rd8(B_PAGE);
		break;
	case 0x10:
		palette(cpu);
		break;
	case 0x11:
		fonts(cpu);
		break;
	case 0x12:
		switch (BL) {
		case 0x10:                          /* EGA information: colour, 256 KB */
			BH = 0;
			BL = 3;
			CH = 0;
			CL = 0x09;
			break;
		case 0x20:                          /* print screen: the alternate routine */
			break;
		case 0x30: case 0x31: case 0x32: case 0x33: case 0x34: case 0x35: case 0x36:
			AL = 0x12;
			break;
		default:
			break;
		}
		break;
	case 0x13: {                            /* write string ES:BP, CX long, at DH,DL */
		int page = BH & 7;
		uint16_t save = g_rd16(B_CURPOS(page));
		g_wr16(B_CURPOS(page), DX);
		uint32_t s = LIN(ES, BP);
		for (int i = 0; i < CX; i++) {
			uint8_t ch = g_rd8(s++), at = BL;
			if (AL & 2)
				at = g_rd8(s++);
			tty_attr(ch, page, at, true);
		}
		if (!(AL & 1))
			g_wr16(B_CURPOS(page), save);
		if (page == g_rd8(B_PAGE))
			cursor_hw();
		break;
	}
	case 0x1A:                              /* display combination: VGA colour */
		if (AL == 0) {
			BX = 0x0008;
		}
		AL = 0x1A;
		break;
	default:
		break;
	}
	return HLE_DONE;
}

void int10_post(PC *pc)
{
	(void)pc;
	set_mode(0x03);
}
