/*
 * FRANK DOS — the FreeDOS kernel's runtime (dosrt.c): what the BIOS and the
 * main loop call. Plain C types only: included outside the port too.
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: MIT
 */
#ifndef FDOS_DOSRT_H
#define FDOS_DOSRT_H

#include <stdbool.h>
#include <stdint.h>

struct CPUI386;

/* the native arena: the top of PSRAM, past the guest's RAM (main.c shrinks
 * the guest by it): the kernel's data (64 KB, linked at 0x117C0000 by
 * boards/memmap_nx_overlay.ld), then the kernel's coroutine stacks */
#define FDOS_ARENA_BYTES (256u * 1024u)
#define FDOS_DATA_BYTES  (64u * 1024u)

void dosrt_psram_init(void);    /* after pc_new(): the kernel's .data, .bss */

void dosrt_init(struct CPUI386 *cpu);
bool dosrt_boot(struct CPUI386 *cpu, uint8_t dl);    /* INT 19h: the kernel boots */
bool dosrt_int(struct CPUI386 *cpu, uint8_t n);      /* a DOS entry stub's trap */
bool dosrt_callback(struct CPUI386 *cpu);            /* the FFEFFh sentinel's trap */
bool dosrt_active(void);                             /* a kernel coroutine waits */
void dosrt_after_batch(void);                        /* the main loop: resume it */
void dosrt_report(void);                             /* the console's ~C regs~ */
/* around a native application's run on its own stack (task.c) */
void *dosrt_native_stack_enter(void);
void dosrt_native_stack_leave(void *old);

#endif
