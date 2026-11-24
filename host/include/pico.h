/* FRANK DOS host build: the Pico SDK's base header, stood in for. */
#ifndef FRANK_HOST_PICO_H
#define FRANK_HOST_PICO_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#define __not_in_flash(group)
#define __not_in_flash_func(f) f
#define __no_inline_not_in_flash_func(f) __attribute__((noinline)) f
#define __time_critical_func(f) f
#define __scratch_x(group)
#define __scratch_y(group)
#define __in_flash(group)
#define __uninitialized_ram(v) v
#ifndef __force_inline
#define __force_inline inline __attribute__((always_inline))
#endif
#ifndef __always_inline
#define __always_inline inline __attribute__((always_inline))
#endif
#ifndef __aligned
#define __aligned(n) __attribute__((aligned(n)))
#endif
#ifndef __packed
#define __packed __attribute__((packed))
#endif
#define __dmb() __sync_synchronize()
#define __dsb() __sync_synchronize()
#define __isb() __sync_synchronize()
#define __wfe() ((void)0)
#define __sev() ((void)0)
#define __wfi() ((void)0)
#define __compiler_memory_barrier() __asm__ volatile("" ::: "memory")
#define tight_loop_contents() ((void)0)
#define get_core_num() 0u
#define __fast_mul(a, b) ((a) * (b))
#define __mul_instruction(a, b) ((a) * (b))

typedef unsigned int uint;

#ifdef __cplusplus
extern "C" {
#endif
void panic(const char *fmt, ...) __attribute__((noreturn));
static inline uint32_t save_and_disable_interrupts(void) { return 0; }
static inline void restore_interrupts(uint32_t s) { (void)s; }
#ifdef __cplusplus
}
#endif
#endif
