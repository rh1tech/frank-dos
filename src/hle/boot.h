/*
 * FRANK DOS — the machine brought up natively: no SeaBIOS, no VGA BIOS.
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: MIT
 */
#ifndef HLE_BOOT_H
#define HLE_BOOT_H

#include <stdbool.h>
#include "pc.h"

/* the guest CPU runs (false until the native BIOS and DOS start it) */
extern bool hle_cpu_running;

/* the native BIOS installed, the CPU reset: POST runs from F000:FFF0 */
void hle_boot(PC *pc);

/* PIT channel 2 at hz into the speaker for ms (the mixer must be running) */
void hle_beep(PC *pc, unsigned hz, unsigned ms);

/* a port write as the guest would make it (the PC's I/O callbacks) */
void hle_outb(PC *pc, int port, uint8_t val);
uint8_t hle_inb(PC *pc, int port);

#endif
