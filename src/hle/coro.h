/*
 * Host coroutines: every guest thread runs on its own host stack, so native
 * code may block anywhere (a wait inside a dialog's message loop inside a
 * window procedure...).  The host uses ucontext; the board switches stacks
 * itself (src/platform/rp2350/coro_rp2350.c).
 */
#ifndef CORO_H
#define CORO_H

#include <stddef.h>

typedef struct coro coro_t;

/* The context that is running now (the program's own stack), as a coroutine. */
coro_t *coro_self_main(void);
/* A coroutine that will run fn(arg) on stack[0..size); fn must never return. */
coro_t *coro_new(void (*fn)(void *), void *arg, void *stack, size_t size);
void coro_free(coro_t *c);
/* frank-dos: c runs without a stack limit (MSPLIM 0) */
void coro_nolimit(coro_t *c);
/* frank-dos: where a suspended coroutine's stack pointer is */
void *coro_sp(coro_t *c);
/* Suspend from, resume to. */
void coro_switch(coro_t *from, coro_t *to);

#endif
