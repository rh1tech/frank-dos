/*
 * FRANK DOS — the SD card as drive H: from the start, as frank-386's
 * MAPDRIVE.COM makes it: H:'s CDS entry a network drive at "H:\", served by
 * the native redirector (netredirect.c, on INT 2Fh/11xxh). Done by the shell
 * before AUTOEXEC.BAT, after CONFIG.SYS's LASTDRIVE has laid out the CDS;
 * a drive H: already in use, or a LASTDRIVE short of it, is left alone.
 * With boot=sd the card is C: instead, and the kernel maps it before
 * CONFIG.SYS too (fdos_map_sd_boot_drive): CONFIG.SYS and AUTOEXEC.BAT come
 * from the card.
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: MIT
 */
#include "hdrs.h"

int frank_printf(const char *fmt, ...);   /* bios_shim.c: the console */

char netredirect_letter(void);          /* H:, or C: with boot=sd */

void fdos_map_sd_drive(void)
{
	extern CPU *cpu;
	if (!cpu || !cpu->int2f_handler)    /* (config.ini redirector=0) */
		return;
#ifdef FRANK_HOST
	if (getenv("FRANK_NO_SDDRIVE"))     /* (the host: MAPDRIVE.COM's own, to compare) */
		return;
#endif
	uint32_t lol = ((uint32_t)DOS_PSP << 4) + 0x08F0u;
	UWORD cds_off = pload16(lol + offsetof(struct lol, CDSp));
	UWORD cds_seg = pload16(lol + offsetof(struct lol, CDSp) + 2);
	UBYTE last = pload8(lol + offsetof(struct lol, lastdrive));
	const unsigned SD_DRIVE = (unsigned)(netredirect_letter() - 'A');
	if (last <= SD_DRIVE || (cds_off == 0xFFFF && cds_seg == 0xFFFF)) {
		frank_printf("sddrive: LASTDRIVE %c: leaves no %c:\n", 'A' + last - 1, netredirect_letter());
		return;
	}
	uint32_t c = ((uint32_t)cds_seg << 4) + cds_off + SD_DRIVE * sizeof(struct cds);
	UWORD flags = pload16(c + offsetof(struct cds, cdsFlags));
	if (flags & CDSVALID)               /* (a real drive, or MAPDRIVE was here first) */
		return;
	const char root[4] = { netredirect_letter(), ':', '\\', 0 };
	for (unsigned i = 0; i < MAX_CDSPATH; i++)
		pstore8(c + offsetof(struct cds, cdsCurrentPath) + i, i < sizeof root ? (UBYTE)root[i] : 0);
	pstore16(c + offsetof(struct cds, cdsFlags), CDSNETWDRV | CDSPHYSDRV);
	pstore16(c + offsetof(struct cds, cdsDpb), 0);
	pstore16(c + offsetof(struct cds, cdsDpb) + 2, 0);
	pstore16(c + offsetof(struct cds, _cdsUnion), 0xFFFF);
	pstore16(c + offsetof(struct cds, _cdsUnion) + 2, 0xFFFF);
	pstore16(c + offsetof(struct cds, cdsStoreUData), 0xFFFF);   /* (as MAPDRIVE.COM leaves it) */
	pstore16(c + offsetof(struct cds, cdsBackslashOffset), 2);
	/* the redirector's SDA, as MAPDRIVE.COM's installation check gives it */
	extern void netredirect_set_sda(uint32_t linear);
	netredirect_set_sda(((uint32_t)DOS_PSP << 4) + X86_INTERNAL_DATA_OFF + offsetof(struct dos_data, ErrorMode));
	frank_printf("sddrive: the SD card is %c:\n", netredirect_letter());
}

/* boot=sd: C: before CONFIG.SYS (kernel.c's init, after the CDS exists) */
void fdos_map_sd_boot_drive(void)
{
	if (netredirect_letter() == 'C')
		fdos_map_sd_drive();
}
