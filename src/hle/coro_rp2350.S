/*
 * frank-react: coroutine stack switch for the RP2350 (Cortex-M33, softfp
 * with the FPU in use): the AAPCS callee-saved core and VFP registers.
 *
 *   void coro_swap(uint32_t **save_sp, uint32_t *new_sp)
 *
 * A new coroutine's stack is laid out as if it had called coro_swap:
 * s16-s31, r4-r11, then the return address (coro_start, with r4 = fn and
 * r5 = arg).
 */
	.syntax unified
	.thumb
	.text

	.global coro_swap
	.type coro_swap, %function
	.thumb_func
coro_swap:
	push {r4-r11, lr}
	vpush {s16-s31}
	mov r2, sp
	str r2, [r0]
	mov sp, r1
	vpop {s16-s31}
	pop {r4-r11, pc}
	.size coro_swap, . - coro_swap

	.global coro_start
	.type coro_start, %function
	.thumb_func
coro_start:
	/* a new coroutine: its stack's limit (r6 = the bottom), then fn(arg) */
	msr msplim, r6
	mov r0, r5
	blx r4
1:	b 1b
	.size coro_start, . - coro_start
