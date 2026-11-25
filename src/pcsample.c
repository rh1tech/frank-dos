/**
 * frank-386 — statistical PC sampling profiler for core 0.
 *
 * SPDX-License-Identifier: MIT
 *
 * The interpreter costs a measured 273 core cycles per guest instruction
 * (docs/C2_SPLIT_PLAN.md §6c) and it is compute-bound, so the useful
 * question is which code inside cpu_exec1's 119 KB actually burns them.
 *
 * Why sampling rather than instrumentation: adding DWT reads to the hot
 * loop measurably slows the machine — that mistake has already been made
 * twice here — and any figure taken from an instrumented build is
 * suspect. A SysTick interrupt that records the interrupted PC costs
 * ~0.1% at 10 kHz and does not distort what it measures.
 *
 * Why not halt over SWD and read PC: halting an RP2350 under OpenOCD
 * clears CPACR, and doing it repeatedly already left this board
 * unreachable once, needing a power cycle. Not something to do
 * unattended.
 *
 * SysTick is per-core and core 1 uses the alarm pool, so core 0's
 * SysTick is free.
 */
#include "pcsample.h"

#if PC_SAMPLE

#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/structs/systick.h"
#include "i386.h"
#include "hardware/regs/addressmap.h"
#include "hardware/regs/xip.h"

/* Histogram over the RAM-resident text region. cpu_exec1 lives at
 * ~0x20002c5c and runs 119 KB; 0x20000000..0x20040000 at 64-byte
 * granularity covers it with room to spare for 16 KB of counters. */
/* (frank-dos: in PSRAM, with the kernel's data: the SRAM heap has no 12 KB to spare) */
uint32_t ps_hist[PS_BUCKETS] __attribute__((section(".psram_bss")));
uint32_t ps_fhist[PS_FBUCKETS] __attribute__((section(".psram_bss")));
volatile uint32_t ps_total __attribute__((used));
volatile uint32_t ps_outside __attribute__((used));

/* Called from the naked handler with the interrupted PC. */
/* frank-dos: and the guest's mode at each sample (a DPMI host's question:
 * how much of a DOS extender's time is in real mode or V86) */
volatile uint32_t ps_real, ps_v86;
extern void *hle_pc;                    /* (hle.c: the PC, its CPU first) */
void __not_in_flash_func(ps_record)(uint32_t pc) {
    ps_total++;
    if (hle_pc) {
        const CPUI386 *cpu = *(CPUI386 *const *)hle_pc;     /* (PC's first field) */
        if (!(cpu->cr0 & 1))
            ps_real++;
        else if (cpu->flags & 0x20000u)
            ps_v86++;
    }
    const uint32_t off = pc - PS_BASE;
    if (off >= PS_SPAN) {
        const uint32_t f = pc - PS_FBASE;
        if (f < PS_FSPAN)
            ps_fhist[f >> PS_FSHIFT]++;
        else
            ps_outside++;
        return;
    }
    ps_hist[off >> PS_SHIFT]++;
}

/*
 * Naked so the exception frame is where the hardware left it. A normal C
 * handler's prologue would push registers first and the stacked PC would
 * no longer be at a known offset.
 *
 * EXC_RETURN bit 2 selects which stack the frame is on; the PC sits at
 * offset 24 of the eight-word hardware frame. r0-r3/r12 are already
 * stacked by the exception, so ps_record may clobber them freely.
 */
__attribute__((naked)) void __not_in_flash_func(isr_systick)(void) {
    __asm volatile(
        "mov   r0, lr            \n"
        "tst   r0, #4            \n"
        "ite   eq                \n"
        "mrseq r0, msp           \n"
        "mrsne r0, psp           \n"
        "ldr   r0, [r0, #24]     \n"   /* stacked PC */
        "push  {r4, lr}          \n"   /* keep 8-byte alignment */
        "bl    ps_record         \n"
        "pop   {r4, lr}          \n"
        "bx    lr                \n"
    );
}

void ps_init(uint32_t sys_hz, uint32_t sample_hz) {
    memset(ps_hist, 0, sizeof(ps_hist));
    ps_total = 0;
    ps_outside = 0;

    uint32_t reload = sys_hz / sample_hz;
    if (reload > 0x00FFFFFFu) reload = 0x00FFFFFFu;   /* 24-bit */
    if (reload < 2u) reload = 2u;

    systick_hw->csr = 0;
    systick_hw->rvr = reload - 1u;
    systick_hw->cvr = 0;
    /* CLKSOURCE=processor | TICKINT | ENABLE */
    systick_hw->csr = (1u << 2) | (1u << 1) | (1u << 0);
}

void ps_stop(void) { systick_hw->csr = 0; }

/* the XIP cache's own counters (every cached flash and PSRAM access, and
 * the hits among them; written to clear) */
#define XIP_CTR_HIT (*(volatile uint32_t *)(XIP_CTRL_BASE + XIP_CTR_HIT_OFFSET))
#define XIP_CTR_ACC (*(volatile uint32_t *)(XIP_CTRL_BASE + XIP_CTR_ACC_OFFSET))

static uint64_t ps_cycle0, ps_us0;          /* the guest's instructions, the time, at ps_clear */

static uint64_t ps_guest_insns(void) {
    if (!hle_pc)
        return 0;
    const CPUI386 *cpu = *(CPUI386 *const *)hle_pc;
    return cpu->cycle;
}

void ps_clear(void) {
    XIP_CTR_HIT = 0;
    XIP_CTR_ACC = 0;
    ps_cycle0 = ps_guest_insns();
    ps_us0 = time_us_64();
    extern uint32_t cpu_stat_pe_switches;
    cpu_stat_pe_switches = 0;
    ps_real = ps_v86 = 0;
    memset(ps_hist, 0, sizeof(ps_hist));
    memset(ps_fhist, 0, sizeof(ps_fhist));
    ps_total = 0;
    ps_outside = 0;
}

/* the hottest buckets, SRAM and flash together: "ps ADDR COUNT" lines, the
 * addresses for addr2line / nm against the ELF */
void ps_report(int top) {
    static uint8_t taken_r[PS_BUCKETS / 8], taken_f[PS_FBUCKETS / 8];
    memset(taken_r, 0, sizeof taken_r);
    memset(taken_f, 0, sizeof taken_f);
    uint32_t total = ps_total, flash = 0;
    for (unsigned i = 0; i < PS_FBUCKETS; i++)
        flash += ps_fhist[i];
    printf("ps: total %lu sram %lu flash %lu other %lu\n", (unsigned long)total,
           (unsigned long)(total - flash - ps_outside), (unsigned long)flash, (unsigned long)ps_outside);
    {
        uint32_t acc = XIP_CTR_ACC, hit = XIP_CTR_HIT;
        printf("ps: XIP cache accesses %lu hits %lu (%lu.%lu%%), misses %lu\n", (unsigned long)acc,
               (unsigned long)hit, (unsigned long)(acc ? (uint64_t)hit * 100u / acc : 0),
               (unsigned long)(acc ? (uint64_t)hit * 1000u / acc % 10 : 0), (unsigned long)(acc - hit));
    }
    {
        uint64_t n = ps_guest_insns() - ps_cycle0, us = time_us_64() - ps_us0;
        uint32_t mhz = clock_get_hz(clk_sys) / 1000000u;
        printf("ps: guest instructions %llu in %llu ms: %lu k/s, %lu.%lu cycles each\n",
               (unsigned long long)n, (unsigned long long)(us / 1000), (unsigned long)(us ? n * 1000u / us : 0),
               (unsigned long)(n ? us * mhz / n : 0), (unsigned long)(n ? us * mhz * 10u / n % 10 : 0));
    }
    extern uint32_t cpu_stat_pe_switches;
    printf("ps: guest real mode %lu.%lu%% v86 %lu.%lu%%, PE switches %lu\n",
           (unsigned long)(ps_real * 100u / (total ? total : 1)), (unsigned long)(ps_real * 1000u / (total ? total : 1) % 10),
           (unsigned long)(ps_v86 * 100u / (total ? total : 1)), (unsigned long)(ps_v86 * 1000u / (total ? total : 1) % 10),
           (unsigned long)cpu_stat_pe_switches);
    for (int k = 0; k < top; k++) {
        uint32_t best = 0, addr = 0;
        int bi = -1, bf = 0;
        for (unsigned i = 0; i < PS_BUCKETS; i++)
            if (!(taken_r[i >> 3] & (1u << (i & 7))) && ps_hist[i] > best) {
                best = ps_hist[i]; bi = (int)i; bf = 0;
            }
        for (unsigned i = 0; i < PS_FBUCKETS; i++)
            if (!(taken_f[i >> 3] & (1u << (i & 7))) && ps_fhist[i] > best) {
                best = ps_fhist[i]; bi = (int)i; bf = 1;
            }
        if (bi < 0 || !best)
            break;
        if (bf) {
            taken_f[bi >> 3] |= 1u << (bi & 7);
            addr = PS_FBASE + ((uint32_t)bi << PS_FSHIFT);
        } else {
            taken_r[bi >> 3] |= 1u << (bi & 7);
            addr = PS_BASE + ((uint32_t)bi << PS_SHIFT);
        }
        printf("ps %08lx %c %lu %lu.%lu%%\n", (unsigned long)addr, bf ? 'f' : 's', (unsigned long)best,
               (unsigned long)(best * 100u / (total ? total : 1)), (unsigned long)(best * 1000u / (total ? total : 1) % 10));
    }
}
#endif /* PC_SAMPLE */
