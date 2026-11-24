/* Host coroutines on ucontext (deprecated on macOS, still there); from frank-react. */
#define _XOPEN_SOURCE 700
#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#endif
#include "hle/coro.h"

#include <stdint.h>
#include <stdlib.h>
#include <ucontext.h>

#pragma clang diagnostic ignored "-Wdeprecated-declarations"

struct coro {
	ucontext_t uc;
	/* macOS's libc keeps the registers (__mcontext_data) right behind the
	 * ucontext_t, which this header's ucontext_t may not include */
	_Alignas(16) unsigned char mcontext_space[1024];
	void (*fn)(void *);
	void *arg;
};

static void trampoline(unsigned hi, unsigned lo)
{
	coro_t *c = (coro_t *)(((uintptr_t)hi << 32) | (uintptr_t)lo);
	c->fn(c->arg);
	abort();                    /* a coroutine never returns */
}

coro_t *coro_self_main(void)
{
	return calloc(1, sizeof(coro_t));
}

coro_t *coro_new(void (*fn)(void *), void *arg, void *stack, size_t size)
{
	coro_t *c = calloc(1, sizeof *c);
	if (!c)
		return NULL;
	c->fn = fn;
	c->arg = arg;
	getcontext(&c->uc);
	c->uc.uc_stack.ss_sp = stack;
	c->uc.uc_stack.ss_size = size;
	c->uc.uc_link = NULL;
	uintptr_t p = (uintptr_t)c;
	makecontext(&c->uc, (void (*)(void))trampoline, 2, (unsigned)(p >> 32), (unsigned)p);
	return c;
}

void coro_free(coro_t *c)
{
	free(c);
}

void coro_switch(coro_t *from, coro_t *to)
{
	swapcontext(&from->uc, &to->uc);
}

/* frank-dos: the board's stack limit and stack pointer */
void coro_nolimit(coro_t *c)
{
	(void)c;
}

void *coro_sp(coro_t *c)
{
#if defined(__APPLE__) && defined(__aarch64__)
	return (void *)(uintptr_t)c->uc.uc_mcontext->__ss.__sp;
#elif defined(__APPLE__) && defined(__x86_64__)
	return (void *)(uintptr_t)c->uc.uc_mcontext->__ss.__rsp;
#elif defined(__linux__) && defined(__aarch64__)
	return (void *)(uintptr_t)c->uc.uc_mcontext.sp;
#elif defined(__linux__) && defined(__x86_64__)
	return (void *)(uintptr_t)c->uc.uc_mcontext.gregs[REG_RSP];
#else
#error "coro_sp: this host's ucontext layout"
#endif
}
