/* The one platform header the i386 core needs (frank-react's src/cpu/cpu_port.h,
 * which the core shares with murm386): on the board the SDK's <pico.h> for the
 * __not_in_flash placement and malloc for the TLB (SRAM: the PSRAM is all guest
 * memory); in tools/cpubench's host and qemu builds the placement attributes
 * are no-ops. The RDTSC clock is cpu_port_time_us() (platform_rp2350.c). */
#ifndef CPU_PORT_H
#define CPU_PORT_H
#include <stddef.h>
#include <stdlib.h>
#if defined(PICO_BUILD) || defined(PICO_RP2350) || defined(PICO_ON_DEVICE)
#include <pico.h>
#else
#define __not_in_flash(group)
#define __not_in_flash_func(f) f
#ifndef likely
#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
#endif
#endif
#define cpu_port_fast_malloc(n) malloc(n)
#endif
