/*
 * frank-386 — the real-mode JIT's hook for memory written without the CPU
 * (DMA, the IDE and INT 13h disks, the H: redirector): a write into a page
 * holding compiled code flushes the cache before the next block runs.
 * See jit16.inc.
 */
#ifndef JIT16_H
#define JIT16_H
#include <stdint.h>
#ifndef JIT16
#define JIT16 0
#endif
/* frank-dos: the PSRAM store of compiled blocks (jit16.inc), below the
 * kernel's data (fdos/dosrt.h): main.c takes it from the guest's RAM when
 * the JIT runs, and tells the JIT with jit_store_init() */
#define JIT_STORE_BYTES (512u * 1024u)
#if JIT16
void jit_note_write(uint32_t addr, uint32_t len);
void jit_store_init(void *base, uint32_t bytes);
#else
static inline void jit_note_write(uint32_t addr, uint32_t len) { (void)addr; (void)len; }
#endif
#ifndef JIT_PROF_PFX
#define JIT_PROF_PFX 0  /* (JIT_PROF: the opcodes after 66/67/26 too; 3 KB the heap lacks with its address ring) */
#endif

#endif
