/**
 * frank-386 — statistical PC sampling profiler for core 0. See pcsample.c.
 */
#ifndef PCSAMPLE_H
#define PCSAMPLE_H

#include <stdint.h>

#if PC_SAMPLE

/* RAM text window. cpu_exec1 sits at ~0x20002c5c and spans 119 KB. */
#define PS_BASE    0x20000000u
#define PS_SPAN    0x00040000u          /* 256 KB */
#define PS_SHIFT   7                    /* 128-byte buckets = 8 KB histogram.
                                        * 64-byte buckets (16 KB) push RAM
                                        * near the point where pc_new() can no
                                        * longer allocate. */
#define PS_BUCKETS (PS_SPAN >> PS_SHIFT)

extern uint32_t ps_hist[PS_BUCKETS];
/* frank-dos: and the XIP flash (the kernel, the BIOS, the devices), coarser */
#define PS_FBASE   0x10000000u
#define PS_FSPAN   0x00200000u          /* 2 MB: FLASH and FLASH_HI's code */
#define PS_FSHIFT  11                   /* 2 KB buckets = 4 KB histogram */
#define PS_FBUCKETS (PS_FSPAN >> PS_FSHIFT)
extern uint32_t ps_fhist[PS_FBUCKETS];
void ps_report(int top);                /* the console's ~C ps~ (~C ps0~: from zero) */
void ps_clear(void);
extern volatile uint32_t ps_total;
extern volatile uint32_t ps_outside;

void ps_init(uint32_t sys_hz, uint32_t sample_hz);
void ps_stop(void);

#else

static inline void ps_init(uint32_t a, uint32_t b) { (void)a; (void)b; }
static inline void ps_stop(void) {}

#endif /* PC_SAMPLE */

#endif /* PCSAMPLE_H */
