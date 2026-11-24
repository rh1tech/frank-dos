/* FRANK DOS host build: the RP2350's SYSINFO block, as an RP2350B's */
#pragma once
#include "pico.h"
typedef volatile uint32_t io_ro_32;
typedef volatile uint32_t io_rw_32;
extern uint32_t host_sysinfo[4];
#define SYSINFO_BASE ((uintptr_t)host_sysinfo)
#define SYSINFO_PACKAGE_SEL_OFFSET 4u
