/*
 * FRANK DOS — the BIOS ROM's layout (segment F000h), shared by rom.asm (as
 * rom_layout.inc, generated from this file) and the native BIOS.
 *
 * The IBM PC/AT entry points stay where software expects them: POST at
 * E05Bh, the reset jump at FFF0h, the 8x8 font's first half at FA6Eh, the
 * diskette parameters at EFC7h, the configuration table at E6F5h.
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: MIT
 */
#ifndef HLE_ROM_LAYOUT_H
#define HLE_ROM_LAYOUT_H

#define ROM_SEG          0xF000
#define ROM_BASE         0xF0000u

#define ROM_VECTORS      0x0000  /* dw[256]: each vector's offset in F000h (0: leave it) */
#define ROM_MOUSE_FN     0x0F00  /* dd: the INT 15h C207h handler (off, seg), written natively */
#define ROM_FONT8X16     0xA000  /* 256 x 16 */
#define ROM_FONT8X14     0xB000  /* 256 x 14 */
#define ROM_FONT8X8      0xC000  /* 256 x 8; INT 1Fh points at its second half */
#define ROM_FONT8X8_HI   0xC400
#define ROM_DOS_STUBS    0xD000  /* 8 bytes per INT: the FreeDOS kernel's entries (TRAP_DOS + n) */
#define ROM_POST         0xE05B
#define ROM_CONFIG_TABLE 0xE6F5  /* INT 15h C0h */
#define ROM_DPT          0xEFC7  /* INT 1Eh: diskette parameters */
#define ROM_FONT8X8_LO   0xFA6E  /* the 8x8 font's first 128 characters */
#define ROM_CALLBACK     0xFEFF  /* FFEFFh: the kernel's return sentinel (FFEF:000F), XMS (F000:FEFF) */
#define ROM_IRET         0xFF06  /* FFF0:0006: a bare IRET (the kernel's empty handler) */
#define ROM_IDLE         0xFF10  /* STI, HLT, again: the guest while the kernel waits */
#define ROM_INT24        0xFF44  /* the default critical error handler: AL=03h (fail) */
#define ROM_NLS          0xFF78  /* FFF0:0078: the country upcase service (RETF) */
#define ROM_RESET        0xFFF0
#define ROM_DATE         0xFFF5
#define ROM_MODEL        0xFFFE

/* trap ids (the word after 0F FF): an INT's native body is its number */
#define TRAP_POST        0x100
#define TRAP_BOOTFAIL    0x101
#define TRAP_CALLBACK    0x102
#define TRAP_DOS         0x200   /* + the INT number */

#endif
