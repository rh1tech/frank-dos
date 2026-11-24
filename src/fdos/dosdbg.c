/*
 * FRANK DOS — the console's look into the kernel's tables ("~C cds~").
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: MIT
 */
#include "hdrs.h"

int frank_printf(const char *fmt, ...);   /* bios_shim.c: the console (the kernel redefines printf) */
#undef printf
#define printf frank_printf

void fdos_dump_cds(void)
{
	uint32_t lol = ((uint32_t)DOS_PSP << 4) + 0x08F0u;
	uint32_t cdsp = lol + offsetof(struct lol, CDSp);
	UWORD cds_off = pload16(cdsp), cds_seg = pload16(cdsp + 2);
	UBYTE nblk = pload8(lol + offsetof(struct lol, nblkdev));
	UBYTE last = pload8(lol + offsetof(struct lol, lastdrive));
	UBYTE boot = pload8(lol + offsetof(struct lol, BootDrive));
	printf("lol %05lx: cds %04x:%04x nblkdev %u lastdrive %u bootdrive %u (cds %u bytes)\n",
	       (unsigned long)lol, cds_seg, cds_off, nblk, last, boot, (unsigned)sizeof(struct cds));
	uint32_t c = ((uint32_t)cds_seg << 4) + cds_off;
	for (int i = 0; i < last && i < 10; i++, c += sizeof(struct cds)) {
		char path[16];
		for (int k = 0; k < 15; k++)
			path[k] = (char)pload8(c + offsetof(struct cds, cdsCurrentPath) + k);
		path[15] = 0;
		printf("  %c: flags %04x dpb %04x:%04x path %s\n", 'A' + i,
		       pload16(c + offsetof(struct cds, cdsFlags)),
		       pload16(c + offsetof(struct cds, cdsDpb) + 2), pload16(c + offsetof(struct cds, cdsDpb)), path);
		if (pload16(c + offsetof(struct cds, cdsFlags)) & CDSNETWDRV) {   /* (a network drive: all of it) */
			printf("    ");
			for (unsigned k = MAX_CDSPATH; k < sizeof(struct cds); k++)
				printf("%02x ", pload8(c + k));
			printf("\n");
		}
	}
}
