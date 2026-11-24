/*
 * FRANK DOS — INT 13h on disk images on the SD card, native.
 *
 * Floppy (00h, 01h) and fixed disk (80h, 81h) images are files read through
 * FatFs: CHS and the extended (LBA) calls, the parameters a DOS asks for.
 * No controller is emulated; what a sector costs is one FatFs read.
 *
 * The images are opened for writing too (read-only if that fails: then a
 * write answers "write protected", 03h). A write goes into the image at once;
 * FatFs's own buffer and the directory entry are flushed by the timer tick
 * (int13_tick) a moment after the last write, not after each sector.
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdlib.h>

#include "ff.h"
#include "pico/time.h"
#include "hle/hle.h"
#include "bulk_bounce.h"
#include "fdos/dosrt.h"

typedef struct {
	FIL f;
	bool open;
	bool ro;                    /* opened read-only */
	bool dirty;                 /* written since the last f_sync */
	bool floppy;
	uint8_t type;               /* the CMOS diskette type (INT 13h 08h's BL) */
	uint32_t sectors;
	uint16_t cyl, heads, spt;
} disk_t;

static disk_t fd[2], hd[2];
static char *img_path[4];       /* fda, fdb, hda, hdb (with their directory) */
static uint8_t last_status[2];  /* floppy, fixed */

static char *join(const char *dir, const char *name)
{
	if (!name || !name[0])
		return NULL;
	size_t n = strlen(dir) + strlen(name) + 2;
	char *p = malloc(n);
	if (p)
		snprintf(p, n, "%s%s%s", dir, dir[0] ? "/" : "", name);
	return p;
}

void int13_set_images(const char *dir, const char *fda, const char *fdb,
		      const char *hda, const char *hdb)
{
	img_path[0] = join(dir, fda);
	img_path[1] = join(dir, fdb);
	img_path[2] = join(dir, hda);
	img_path[3] = join(dir, hdb);
}

static uint32_t last_write_ms;

static bool write_lba(disk_t *d, uint32_t lba, const void *buf)
{
	UINT put;
	if (f_lseek(&d->f, (FSIZE_t)lba * 512) != FR_OK ||
	    f_write(&d->f, buf, 512, &put) != FR_OK || put != 512)
		return false;
	d->dirty = true;
	last_write_ms = to_ms_since_boot(get_absolute_time());
	return true;
}

/* (INT 08h, every tick) the images written to are flushed once writing has
 * paused for a quarter of a second */
void int13_tick(void)
{
	if (!last_write_ms || to_ms_since_boot(get_absolute_time()) - last_write_ms < 250)
		return;
	last_write_ms = 0;
	disk_t *all[4] = { &fd[0], &fd[1], &hd[0], &hd[1] };
	for (int i = 0; i < 4; i++)
		if (all[i]->open && all[i]->dirty) {
			f_sync(&all[i]->f);
			all[i]->dirty = false;
		}
}

static bool read_lba(disk_t *d, uint32_t lba, void *buf)
{
	UINT got;
	return f_lseek(&d->f, (FSIZE_t)lba * 512) == FR_OK &&
	       f_read(&d->f, buf, 512, &got) == FR_OK && got == 512;
}

static void floppy_geometry(disk_t *d)
{
	static const struct { uint32_t kb; uint16_t c, h, s; uint8_t t; } g[] = {
		{160, 40, 1, 8, 1}, {180, 40, 1, 9, 1}, {320, 40, 2, 8, 1}, {360, 40, 2, 9, 1},
		{720, 80, 2, 9, 3}, {1200, 80, 2, 15, 2}, {1440, 80, 2, 18, 4}, {2880, 80, 2, 36, 5},
	};
	uint32_t kb = d->sectors / 2;
	for (unsigned i = 0; i < sizeof g / sizeof g[0]; i++)
		if (g[i].kb == kb) {
			d->cyl = g[i].c;
			d->heads = g[i].h;
			d->spt = g[i].s;
			d->type = g[i].t;
			return;
		}
	d->cyl = 80;
	d->heads = 2;
	d->spt = 18;
	d->type = 4;
}

/* the geometry the partitions were made with (their end CHS), else 16/63 */
static void hd_geometry(disk_t *d)
{
	uint8_t *mbr = guest_bulk_buf;
	d->heads = 16;
	d->spt = 63;
	if (read_lba(d, 0, mbr) && mbr[510] == 0x55 && mbr[511] == 0xAA) {
		for (int i = 0; i < 4; i++) {
			const uint8_t *e = mbr + 446 + 16 * i;
			if (e[4] && (e[6] & 63)) {
				d->heads = e[5] + 1;
				d->spt = e[6] & 63;
				break;
			}
		}
	}
	uint32_t c = d->sectors / ((uint32_t)d->heads * d->spt);
	d->cyl = c > 1024 ? 1024 : (uint16_t)c;
}

static bool open_image(disk_t *d, const char *path, bool floppy)
{
	memset(d, 0, sizeof *d);
	if (!path)
		return false;
	if (f_open(&d->f, path, FA_READ | FA_WRITE) != FR_OK) {
		if (f_open(&d->f, path, FA_READ) != FR_OK) {
			printf("int13: no image %s\n", path);
			return false;
		}
		d->ro = true;
	}
	d->open = true;
	d->floppy = floppy;
	d->sectors = (uint32_t)(f_size(&d->f) / 512);
	if (floppy)
		floppy_geometry(d);
	else
		hd_geometry(d);
	printf("int13: %s %s: %lu sectors, CHS %u/%u/%u%s\n", floppy ? "floppy" : "disk", path,
	       (unsigned long)d->sectors, d->cyl, d->heads, d->spt, d->ro ? " (read-only)" : "");
	return true;
}

void int13_post(PC *pc)
{
	(void)pc;
	for (int i = 0; i < 2; i++) {
		if (fd[i].open)
			f_close(&fd[i].f);
		if (hd[i].open)
			f_close(&hd[i].f);
	}
	int nfd = open_image(&fd[0], img_path[0], true) + open_image(&fd[1], img_path[1], true);
	int nhd = open_image(&hd[0], img_path[2], false) + open_image(&hd[1], img_path[3], false);
	uint16_t eq = g_rd16(BDA(0x10)) & ~0x00C1;
	if (nfd)
		eq |= 0x0001 | ((nfd - 1) << 6);
	g_wr16(BDA(0x10), eq);
	g_wr8(BDA(0x75), nhd);
	/* CMOS 10h: the diskette types (DOS's own drivers read it) */
	outb(0x70, 0x10);
	outb(0x71, (uint8_t)(fd[0].type << 4 | fd[1].type));
}

int int13_boot_drive(void)
{
	if (fd[0].open)
		return 0x00;
	if (hd[0].open)
		return 0x80;
	return -1;
}

static disk_t *drive(uint8_t dl)
{
	if (dl < 2)
		return fd[dl].open ? &fd[dl] : NULL;
	if (dl >= 0x80 && dl < 0x82)
		return hd[dl - 0x80].open ? &hd[dl - 0x80] : NULL;
	return NULL;
}

/* count sectors from lba into ES:BX-style linear dst; returns those done */
static int xfer_read(disk_t *d, uint32_t lba, int count, uint32_t dst)
{
	int done = 0;
	while (done < count) {
		if (lba + done >= d->sectors || !read_lba(d, lba + done, guest_bulk_buf))
			break;
		g_write(dst + done * 512u, guest_bulk_buf, 512);
		done++;
	}
	return done;
}

/* count sectors from linear src into the image at lba; returns those done */
static int xfer_write(disk_t *d, uint32_t lba, int count, uint32_t src)
{
	int done = 0;
	while (done < count) {
		if (lba + done >= d->sectors)
			break;
		g_read(guest_bulk_buf, src + done * 512u, 512);
		if (!write_lba(d, lba + done, guest_bulk_buf))
			break;
		done++;
	}
	return done;
}

int int13_dbg_left;                         /* (console "~C dbg13 N~": the next N requests logged) */

static enum hle_result finish(CPUI386 *cpu, uint8_t dl, uint8_t status)
{
	if (int13_dbg_left > 0) {
		int13_dbg_left--;
		printf("int13 dl %02x -> %02x\n", dl, status);
	}
	last_status[dl >= 0x80] = status;
	if (dl < 0x80)
		g_wr8(BDA(0x41), status);
	else
		g_wr8(BDA(0x74), status);
	AH = status;
	hle_cf(cpu, status != 0);
	return HLE_DONE;
}

enum hle_result hle_int13(CPUI386 *cpu)
{
	uint8_t dl = DL;
	disk_t *d = drive(dl);
	if (int13_dbg_left > 0)
		printf("int13 ax %04x bx %04x cx %04x dx %04x es %04x ds:si %04x:%04x\n", AX, BX, CX, DX, ES, DS, SI);
	switch (AH) {
	case 0x00:                              /* reset */
	case 0x0D:
		return finish(cpu, dl, d || dl < 0x80 ? 0 : 0x01);
	case 0x01:
		AL = last_status[dl >= 0x80];
		return finish(cpu, dl, 0);
	case 0x02:                              /* read CHS */
	case 0x04: {                            /* verify */
		if (!d)
			return finish(cpu, dl, dl < 0x80 ? 0x80 : 0x01);
		unsigned c = CH | ((CL & 0xC0) << 2), h = DH, s = CL & 0x3F;
		int n = AL;
		if (!s || s > d->spt || h >= d->heads || !n)
			return finish(cpu, dl, 0x04);
		uint32_t lba = ((uint32_t)c * d->heads + h) * d->spt + s - 1;
		int done = n;
		if (AH == 0x02)
			done = xfer_read(d, lba, n, LIN(ES, BX));
		AL = (uint8_t)done;
		return finish(cpu, dl, done == n ? 0 : 0x04);
	}
	case 0x03: {                            /* write CHS */
		if (!d)
			return finish(cpu, dl, dl < 0x80 ? 0x80 : 0x01);
		if (d->ro) {
			AL = 0;
			return finish(cpu, dl, 0x03);
		}
		unsigned c = CH | ((CL & 0xC0) << 2), h = DH, s = CL & 0x3F;
		int n = AL;
		if (!s || s > d->spt || h >= d->heads || !n)
			return finish(cpu, dl, 0x04);
		uint32_t lba = ((uint32_t)c * d->heads + h) * d->spt + s - 1;
		int done = xfer_write(d, lba, n, LIN(ES, BX));
		AL = (uint8_t)done;
		return finish(cpu, dl, done == n ? 0 : 0x04);
	}
	case 0x05:                              /* format a track: not here */
		if (!d)
			return finish(cpu, dl, dl < 0x80 ? 0x80 : 0x01);
		AL = 0;
		return finish(cpu, dl, d->ro ? 0x03 : 0x01);
	case 0x08: {                            /* parameters */
		if (!d) {
			if (dl < 0x80) {                /* a drive the machine lacks */
				AX = 0;
				BX = 0;
				CX = 0;
				DX = 0;
				return finish(cpu, dl, 0);
			}
			return finish(cpu, dl, 0x07);
		}
		unsigned maxc = d->cyl - 1;
		CH = maxc & 0xFF;
		CL = (uint8_t)(((maxc >> 2) & 0xC0) | d->spt);
		DH = (uint8_t)(d->heads - 1);
		if (d->floppy) {
			BL = d->type;
			BH = 0;
			DL = (uint8_t)((fd[0].open) + (fd[1].open));
			seg_set(cpu, HS_ES, ROM_SEG);
			DI = ROM_DPT;
		} else
			DL = g_rd8(BDA(0x75));
		AX = 0;
		return finish(cpu, dl, 0);
	}
	case 0x09: case 0x0C: case 0x10: case 0x11: case 0x14:
		return finish(cpu, dl, d ? 0 : 0x01);
	case 0x15:                              /* type */
		if (!d) {
			AH = 0;
			hle_cf(cpu, dl >= 0x80);
			return HLE_DONE;
		}
		if (d->floppy)
			AH = 0x02;                      /* with change line */
		else {
			AH = 0x03;
			CX = (uint16_t)(d->sectors >> 16);
			DX = (uint16_t)d->sectors;
		}
		hle_cf(cpu, false);
		return HLE_DONE;
	case 0x16:                              /* change line: never changed */
		return finish(cpu, dl, d ? 0 : 0x80);
	case 0x41:                              /* extensions */
		if (BX != 0x55AA || !d || d->floppy)
			return finish(cpu, dl, 0x01);
		BX = 0xAA55;
		AH = 0x21;
		CX = 0x0001;
		last_status[1] = 0;
		hle_cf(cpu, false);
		return HLE_DONE;
	case 0x42: case 0x43: case 0x44: {      /* extended read / write / verify, DS:SI the packet */
		if (!d || d->floppy)
			return finish(cpu, dl, 0x01);
		uint32_t p = LIN(DS, SI);
		uint16_t n = g_rd16(p + 2);
		uint32_t buf = LIN(g_rd16(p + 6), g_rd16(p + 4));
		uint32_t lba = g_rd32(p + 8);
		if (AH == 0x43 && d->ro) {
			g_wr16(p + 2, 0);
			return finish(cpu, dl, 0x03);
		}
		int done = AH == 0x42 ? xfer_read(d, lba, n, buf) : AH == 0x43 ? xfer_write(d, lba, n, buf) : n;
		g_wr16(p + 2, (uint16_t)done);
		return finish(cpu, dl, done == n ? 0 : 0x04);
	}
	case 0x48: {                            /* extended parameters */
		if (!d || d->floppy)
			return finish(cpu, dl, 0x01);
		uint32_t p = LIN(DS, SI);
		uint16_t size = g_rd16(p);
		if (size < 26)
			return finish(cpu, dl, 0x01);
		g_wr16(p, 26);
		g_wr16(p + 2, 0x0002);              /* CHS valid */
		g_wr32(p + 4, d->cyl);
		g_wr32(p + 8, d->heads);
		g_wr32(p + 12, d->spt);
		g_wr32(p + 16, d->sectors);
		g_wr32(p + 20, 0);
		g_wr16(p + 24, 512);
		return finish(cpu, dl, 0);
	}
	default:
		return finish(cpu, dl, 0x01);
	}
}

bool hle_native_dos = true;     /* INT 19h starts the FreeDOS kernel (else: the boot sector) */
bool hle_boot_sd;               /* boot=sd: the kernel from "drive 80h", the SD card as C: */

/* INT 19h: the boot sector to 0000:7C00, DL its drive; AX 0 if one was found.
 * With the native DOS, the kernel starts instead, on the same drive. */
enum hle_result hle_int19(CPUI386 *cpu)
{
	if (hle_native_dos) {
		int drv = hle_boot_sd ? 0x80 : int13_boot_drive();
		printf("int19: native FreeDOS from drive %02x\n", drv < 0 ? 0 : drv);
		cpui386_load_seg(cpu, HS_SS, 0);
		SP = 0x7C00;
		if (dosrt_boot(cpu, (uint8_t)(drv < 0 ? 0 : drv)))
			return HLE_JUMPED;
		printf("int19: the kernel did not start\n");
	}
	static const uint8_t order[] = {0x00, 0x80};
	for (unsigned i = 0; i < sizeof order; i++) {
		disk_t *d = drive(order[i]);
		if (!d || !read_lba(d, 0, guest_bulk_buf))
			continue;
		if (!d->floppy && (guest_bulk_buf[510] != 0x55 || guest_bulk_buf[511] != 0xAA))
			continue;
		g_write(0x7C00, guest_bulk_buf, 512);
		DX = order[i];
		AX = 0;
		printf("int19: booting drive %02x\n", order[i]);
		return HLE_DONE;
	}
	AX = 1;
	return HLE_DONE;
}

enum hle_result hle_int18(CPUI386 *cpu)
{
	(void)cpu;
	int10_puts("No bootable disk.\r\n");
	printf("int18: no bootable disk\n");
	return HLE_DONE;
}
