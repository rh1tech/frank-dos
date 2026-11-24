/* frank-react: coroutines on the RP2350 (coro_rp2350.S switches stacks). */
#include "hle/coro.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct coro {
	uint32_t *sp;
	uint32_t msplim;            /* the stack limit while it runs (0: none) */
};

/* ARMv8-M stack limit: the SDK guards core 0's stack (in SRAM) with MSPLIM;
 * coroutine stacks are in PSRAM, below it, so each context has its own */
static inline uint32_t get_msplim(void)
{
	uint32_t v;
	__asm volatile("mrs %0, msplim" : "=r"(v));
	return v;
}

static inline void set_msplim(uint32_t v)
{
	__asm volatile("msr msplim, %0" : : "r"(v) : "memory");
}

void coro_swap(uint32_t **save_sp, uint32_t *new_sp);
void coro_start(void);

coro_t *coro_self_main(void)
{
	coro_t *c = calloc(1, sizeof(coro_t));
	if (c)
		c->msplim = get_msplim();
	return c;
}

coro_t *coro_new(void (*fn)(void *), void *arg, void *stack, size_t size)
{
	coro_t *c = calloc(1, sizeof *c);
	if (!c)
		return NULL;
	uint32_t *sp = (uint32_t *)(((uintptr_t)stack + size) & ~(uintptr_t)7);
	/* the frame coro_swap pops: s16-s31, r4-r11, pc */
	sp -= 16 + 8 + 1;
	memset(sp, 0, (16 + 8 + 1) * 4);
	sp[16 + 0] = (uint32_t)(uintptr_t)fn;      /* r4 */
	sp[16 + 1] = (uint32_t)(uintptr_t)arg;     /* r5 */
	sp[16 + 2] = (uint32_t)(uintptr_t)stack;   /* r6: the stack's bottom (MSPLIM) */
	sp[16 + 8] = (uint32_t)(uintptr_t)coro_start | 1u;
	c->sp = sp;
	c->msplim = (uint32_t)(uintptr_t)stack;
	return c;
}

/* frank-dos: no stack limit for c (code it runs moves to stacks of its own) */
void coro_nolimit(coro_t *c)
{
	c->msplim = 0;
	c->sp[16 + 2] = 0;              /* (r6: coro_start's MSPLIM) */
}

void *coro_sp(coro_t *c)
{
	return c->sp;
}

void coro_free(coro_t *c)
{
	free(c);
}

void coro_switch(coro_t *from, coro_t *to)
{
	/* no limit across the switch; then the one of whoever runs */
	set_msplim(0);
	coro_swap(&from->sp, to->sp);
	set_msplim(from->msplim);
}
