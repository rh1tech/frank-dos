/*
 * The fast loop's kernel on the Cortex-M33: the commonest instructions in
 * Thumb-2, the state the C loop keeps in memory pinned in registers.
 *
 * fastloop.inc calls fx_kernel(cpu, st) at the top of each round with an
 * instruction inside the code page window.  It runs instructions until one
 * it doesn't take (another opcode, a prefix, a TLB miss: the TLB tag check
 * is fastloop.inc's FX_MEM, here without its fill), the window's end, or a
 * taken branch once the slice is done; st->pc is then the instruction not
 * run, the C loop's to do.  Nothing is changed before that decision, so
 * the C loop starts it afresh.  The results, lazy flags included, are the
 * C loop's: tools/cpubench fuzzes the two against the original core.
 *
 *   r4  cpu            r8  st->wlo (the window: [wlo, wend))
 *   r5  pc             r9  cpu->tlb.tab
 *   r6  st->wend       r10 fx_ktab
 *   r7  st->stepcount  r11 st->fb (eip = host address - fb)
 *   r2  a cursor in the instruction; r0 r1 r3 r12 lr scratch
 */
	.syntax unified
	.cpu cortex-m33
	.thumb

	/* CPUI386 (i386.h; fastloop.inc asserts these) */
	.equ OFF_ESP, 16
	.equ OFF_FLAGS, 40
	.equ OFF_CC_OP, 264
	.equ OFF_CC_DST, 268
	.equ OFF_CC_DST2, 272
	.equ OFF_CC_SRC1, 276
	.equ OFF_CC_SRC2, 280
	.equ OFF_CC_MASK, 284
	.equ OFF_TLB_TAB, 292
	.equ TLB_RTAG, 0
	.equ TLB_WTAG, 4
	.equ TLB_HBASE, 8
	/* struct fx_state */
	.equ ST_PC, 0
	.equ ST_WLO, 4
	.equ ST_WEND, 8
	.equ ST_FB, 12
	.equ ST_STEPS, 16
	/* lazy flags (i386.c) */
	.equ CC_ADD, 1
	.equ CC_SUB, 3
	.equ CC_DEC32, 9
	.equ CC_INC32, 12
	.equ CC_IMUL32, 15
	.equ CC_MUL32, 18
	.equ CC_SAR, 19
	.equ CC_SHL, 20
	.equ CC_SHR, 21
	.equ CC_AND, 26
	.equ CC_OR, 27
	.equ CC_XOR, 28
	.equ M_ALL, 0x8d5
	.equ M_LOGIC, 0x8c5
	.equ M_INCDEC, 0x8d4
	.equ CF, 0x01
	.equ ZF, 0x40
	.equ SF, 0x80

/* the next instruction, straight on (r2: its first byte) */
.macro NEXT
	mov	r5, r2
	subs	r7, #1
	cmp	r5, r6
	bhs	out
	ldrb	r0, [r5]
	ldr	pc, [r10, r0, lsl #2]
.endm

/* a taken branch to r2: the slice ends there, or the window */
.macro JUMP
	mov	r5, r2
	subs	r7, #1
	ble	out
	cmp	r5, r8
	blo	out
	cmp	r5, r6
	bhs	out
	ldrb	r0, [r5]
	ldr	pc, [r10, r0, lsl #2]
.endm

/* ModRM at r2, 32-bit addressing: r0 = ModRM, r1 = the offset, r2 past it
 * all; mod 3 goes to \reg (r2 past the ModRM).  r3 r12 lr clobbered */
.macro EA reg
	ldrb	r0, [r2], #1
	lsrs	r3, r0, #6
	cmp	r3, #3
	beq	\reg
	and	r12, r0, #7
	tbb	[pc, r3]
0:	.byte	(10f - 0b) / 2, (11f - 0b) / 2, (12f - 0b) / 2, (10f - 0b) / 2
10:	cmp	r12, #4                     /* mod 0 */
	beq	3f
	cmp	r12, #5
	beq	15f
	ldr	r1, [r4, r12, lsl #2]
	b	9f
15:	ldr	r1, [r2], #4
	b	9f
11:	cmp	r12, #4                     /* mod 1 */
	beq	3f
	ldr	r1, [r4, r12, lsl #2]
	ldrsb	r12, [r2], #1
	add	r1, r12
	b	9f
12:	cmp	r12, #4                     /* mod 2 */
	beq	3f
	ldr	r1, [r4, r12, lsl #2]
	ldr	r12, [r2], #4
	add	r1, r12
	b	9f
3:	ldrb	r12, [r2], #1               /* SIB */
	and	lr, r12, #7
	cmp	lr, #5
	bne	6f
	cbnz	r3, 6f
	ldr	r1, [r2], #4
	b	7f
6:	ldr	r1, [r4, lr, lsl #2]
7:	ubfx	lr, r12, #3, #3
	cmp	lr, #4
	beq	8f
	ldr	lr, [r4, lr, lsl #2]
	lsrs	r12, r12, #6
	lsl	lr, lr, r12
	add	r1, lr
8:	cmp	r3, #1
	beq	5f
	cmp	r3, #2
	bne	9f
	ldr	r12, [r2], #4
	add	r1, r12
	b	9f
5:	ldrsb	r12, [r2], #1
	add	r1, r12
9:
.endm

/* linear \rea to the host's, for \size bytes that the TLB tag \tag allows;
 * else (a miss, across pages) to \miss.  r3 r12 lr clobbered */
.macro TLB rea, size, tag, miss
	ubfx	r3, \rea, #12, #9
	add	r3, r9, r3, lsl #5
	ldr	r12, [r3, #\tag]
	add	lr, \rea, #(\size - 1)
	cmp	r12, lr, lsr #12
	bne	\miss
	ldr	r12, [r3, #TLB_HBASE]
	add	\rea, r12
.endm

/* x (r3) op y (r12): the lazy flags; the result to [r1] (not CMP) */
.macro OPCALC kind
	.ifc \kind,add
	add	lr, r3, r12
	strd	r3, r12, [r4, #OFF_CC_SRC1]
	movs	r3, #CC_ADD
	.endif
	.ifc \kind,sub
	sub	lr, r3, r12
	strd	r3, r12, [r4, #OFF_CC_SRC1]
	movs	r3, #CC_SUB
	.endif
	.ifc \kind,cmp
	sub	lr, r3, r12
	strd	r3, r12, [r4, #OFF_CC_SRC1]
	movs	r3, #CC_SUB
	.endif
	.ifc \kind,and
	and	lr, r3, r12
	movs	r3, #CC_AND
	.endif
	.ifc \kind,or
	orr	lr, r3, r12
	movs	r3, #CC_OR
	.endif
	.ifc \kind,xor
	eor	lr, r3, r12
	movs	r3, #CC_XOR
	.endif
	strd	r3, lr, [r4, #OFF_CC_OP]
	.ifc \kind,add
	movw	r3, #M_ALL
	.endif
	.ifc \kind,sub
	movw	r3, #M_ALL
	.endif
	.ifc \kind,cmp
	movw	r3, #M_ALL
	.endif
	.ifc \kind,and
	movw	r3, #M_LOGIC
	.endif
	.ifc \kind,or
	movw	r3, #M_LOGIC
	.endif
	.ifc \kind,xor
	movw	r3, #M_LOGIC
	.endif
	str	r3, [r4, #OFF_CC_MASK]
	.ifnc \kind,cmp
	str	lr, [r1]
	.endif
.endm

/* a 32-bit CMP's lazy flags, x (r3) y (r12) kept */
.macro OPCALCCMP
	sub	lr, r3, r12
	strd	r3, r12, [r4, #OFF_CC_SRC1]
	mov	r0, #CC_SUB
	strd	r0, lr, [r4, #OFF_CC_OP]
	movw	r0, #M_ALL
	str	r0, [r4, #OFF_CC_MASK]
.endm

/* after a 32-bit CMP (x r3, y r12): a Jcc rel8 next is decided here from the
 * ARM compare of the same operands (all but JP/JNP), two instructions done */
.macro CMPNEXT
	cmp	r2, r6
	bhs	68f
	ldrb	r0, [r2]
	sub	r0, r0, #0x70
	cmp	r0, #15
	bhi	68f
	ldrsb	r1, [r2, #1]
	cmp	r3, r12
	tbb	[pc, r0]
49:	.byte	(50f - 49b) / 2, (51f - 49b) / 2, (52f - 49b) / 2, (53f - 49b) / 2
	.byte	(54f - 49b) / 2, (55f - 49b) / 2, (56f - 49b) / 2, (57f - 49b) / 2
	.byte	(58f - 49b) / 2, (59f - 49b) / 2, (68f - 49b) / 2, (68f - 49b) / 2
	.byte	(62f - 49b) / 2, (63f - 49b) / 2, (64f - 49b) / 2, (65f - 49b) / 2
	.align	1
50:	bvs	66f
	b	67f
51:	bvc	66f
	b	67f
52:	blo	66f
	b	67f
53:	bhs	66f
	b	67f
54:	beq	66f
	b	67f
55:	bne	66f
	b	67f
56:	bls	66f
	b	67f
57:	bhi	66f
	b	67f
58:	bmi	66f
	b	67f
59:	bpl	66f
	b	67f
62:	blt	66f
	b	67f
63:	bge	66f
	b	67f
64:	ble	66f
	b	67f
65:	bgt	66f
67:	add	r2, #2                      /* not taken */
	subs	r7, #1
	NEXT
66:	add	r2, #2                      /* taken */
	add	r2, r1
	subs	r7, #1
	JUMP
68:	NEXT
.endm

/* ALU Ev,Gv */
.macro ALU_EVGV kind
	add	r2, r5, #1
	EA	20f
	.ifc \kind,cmp
	TLB	r1, 4, TLB_RTAG, out
	.else
	TLB	r1, 4, TLB_WTAG, out
	.endif
	ldr	r3, [r1]
	b	21f
20:	and	r12, r0, #7
	add	r1, r4, r12, lsl #2
	ldr	r3, [r1]
21:	ubfx	r12, r0, #3, #3
	ldr	r12, [r4, r12, lsl #2]
	.ifc \kind,cmp
	OPCALCCMP
	CMPNEXT
	.else
	OPCALC	\kind
	NEXT
	.endif
.endm

/* ALU Gv,Ev */
.macro ALU_GVEV kind
	add	r2, r5, #1
	EA	20f
	TLB	r1, 4, TLB_RTAG, out
	ldr	r12, [r1]
	b	21f
20:	and	r12, r0, #7
	ldr	r12, [r4, r12, lsl #2]
21:	ubfx	r1, r0, #3, #3
	add	r1, r4, r1, lsl #2
	ldr	r3, [r1]
	.ifc \kind,cmp
	OPCALCCMP
	CMPNEXT
	.else
	OPCALC	\kind
	NEXT
	.endif
.endm

/* group 1, Ev with imm8 (83) or imm32 (81); ADC SBB: the C loop */
.macro GRP1 imm32
	ldrb	r0, [r5, #1]
	ubfx	r3, r0, #3, #3
	sub	r3, #2
	cmp	r3, #1
	bls	out
	add	r2, r5, #1
	EA	20f
	ubfx	r3, r0, #3, #3
	cmp	r3, #7
	beq	22f
	TLB	r1, 4, TLB_WTAG, out
	b	23f
22:	TLB	r1, 4, TLB_RTAG, out
23:	ldr	r3, [r1]
	b	24f
20:	and	r12, r0, #7
	add	r1, r4, r12, lsl #2
	ldr	r3, [r1]
24:	.if \imm32
	ldr	r12, [r2], #4
	.else
	ldrsb	r12, [r2], #1
	.endif
	ubfx	lr, r0, #3, #3
	tbb	[pc, lr]
25:	.byte	(40f - 25b) / 2, (41f - 25b) / 2, (40f - 25b) / 2, (40f - 25b) / 2
	.byte	(44f - 25b) / 2, (45f - 25b) / 2, (46f - 25b) / 2, (47f - 25b) / 2
	.align	1
40:	OPCALC	add
	NEXT
41:	OPCALC	or
	NEXT
44:	OPCALC	and
	NEXT
45:	OPCALC	sub
	NEXT
46:	OPCALC	xor
	NEXT
47:	OPCALCCMP
	CMPNEXT
.endm

/* Jcc on ZF or SF (\bit), taken when set (\set) or clear; \len 2 (rel8) or
 * 6 (0F 8x rel32) */
.macro JZS bit, set, len
	.if \len == 2
	ldrsb	r0, [r5, #1]
	.else
	ldr	r0, [r5, #2]
	.endif
	add	r2, r5, #\len
	ldr	r1, [r4, #OFF_CC_MASK]
	tst	r1, #\bit
	beq	20f
	ldr	r1, [r4, #OFF_CC_DST]
	cmp	r1, #0
	.if \bit == ZF
	.if \set
	beq	22f
	.else
	bne	22f
	.endif
	.else
	.if \set
	blt	22f
	.else
	bge	22f
	.endif
	.endif
	NEXT
20:	ldr	r1, [r4, #OFF_FLAGS]
	tst	r1, #\bit
	.if \set
	bne	22f
	.else
	beq	22f
	.endif
	NEXT
22:	add	r2, r0
	JUMP
.endm

/* Jcc after a CMP/SUB (all six flags pending: the operands' ARM compare,
 * \subcc) or a logic op (CF OF clear: dst against 0, \logcc); else the C
 * loop's */
.macro JREL subcc, logcc, len
	.if \len == 2
	ldrsb	r0, [r5, #1]
	.else
	ldr	r0, [r5, #2]
	.endif
	add	r2, r5, #\len
	ldr	r1, [r4, #OFF_CC_MASK]
	movw	r3, #M_ALL
	cmp	r1, r3
	bne	30f
	ldr	r3, [r4, #OFF_CC_OP]
	cmp	r3, #CC_SUB
	bne	out
	ldr	r1, [r4, #OFF_CC_SRC1]
	ldr	r3, [r4, #OFF_CC_SRC2]
	cmp	r1, r3
	b\subcc	32f
	NEXT
30:	movw	r3, #M_LOGIC
	cmp	r1, r3
	bne	out
	ldr	r3, [r4, #OFF_CC_OP]
	sub	r3, #CC_AND
	cmp	r3, #2
	bhi	out
	ldr	r1, [r4, #OFF_CC_DST]
	cmp	r1, #0
	b\logcc	32f
	NEXT
32:	add	r2, r0
	JUMP
.endm

/* \rd = the address of byte register \ri (AL CL DL BL AH CH DH BH) */
.macro R8 rd, ri
	and	\rd, \ri, #3
	add	\rd, r4, \rd, lsl #2
	add	\rd, \rd, \ri, lsr #2
.endm

/* INC/DEC's CF: the lazy one into cpu->flags (cfsync) */
.macro CFSYNC
	bl	cfsync
.endm

/* 8-bit x (r3) op y (r12), the operands as cpu_exec1 keeps them (sign
 * extended); the result's low byte to [r1] (not CMP) */
.macro OPCALC8 kind
	sxtb	r3, r3
	sxtb	r12, r12
	.ifc \kind,add
	add	lr, r3, r12
	.endif
	.ifc \kind,sub
	sub	lr, r3, r12
	.endif
	.ifc \kind,cmp
	sub	lr, r3, r12
	.endif
	.ifc \kind,and
	and	lr, r3, r12
	.endif
	.ifc \kind,or
	orr	lr, r3, r12
	.endif
	.ifc \kind,xor
	eor	lr, r3, r12
	.endif
	sxtb	lr, lr
	.ifc \kind,add
	strd	r3, r12, [r4, #OFF_CC_SRC1]
	movs	r3, #CC_ADD
	movw	r12, #M_ALL
	.endif
	.ifc \kind,sub
	strd	r3, r12, [r4, #OFF_CC_SRC1]
	movs	r3, #CC_SUB
	movw	r12, #M_ALL
	.endif
	.ifc \kind,cmp
	strd	r3, r12, [r4, #OFF_CC_SRC1]
	movs	r3, #CC_SUB
	movw	r12, #M_ALL
	.endif
	.ifc \kind,and
	movs	r3, #CC_AND
	movw	r12, #M_LOGIC
	.endif
	.ifc \kind,or
	movs	r3, #CC_OR
	movw	r12, #M_LOGIC
	.endif
	.ifc \kind,xor
	movs	r3, #CC_XOR
	movw	r12, #M_LOGIC
	.endif
	strd	r3, lr, [r4, #OFF_CC_OP]
	str	r12, [r4, #OFF_CC_MASK]
	.ifnc \kind,cmp
	strb	lr, [r1]
	.endif
.endm

/* ALU eAX,imm32 */
.macro ACC32 kind
	add	r1, r4, #0
	ldr	r3, [r4]
	ldr	r12, [r5, #1]
	add	r2, r5, #5
	.ifc \kind,cmp
	OPCALCCMP
	CMPNEXT
	.else
	OPCALC	\kind
	NEXT
	.endif
.endm

#if FX_SRAM
	.section .time_critical.fx_kernel, "ax", %progbits
#else
	.section .text.fx_kernel, "ax", %progbits	/* murm386: flash (fastloop.inc's FX_SRAM) */
#endif
	.align	2
	.global	fx_kernel
	.thumb_func
	.type	fx_kernel, %function
fx_kernel:
	push	{r1, r4-r11, lr}
	mov	r4, r0
	ldr	r5, [r1, #ST_PC]
	ldr	r8, [r1, #ST_WLO]
	ldr	r6, [r1, #ST_WEND]
	ldr	r11, [r1, #ST_FB]
	ldr	r7, [r1, #ST_STEPS]
	ldr	r9, [r4, #OFF_TLB_TAB]
	ldr	r10, =fx_ktab
	ldrb	r0, [r5]
	ldr	pc, [r10, r0, lsl #2]

	.global	fx_kernel_out
fx_kernel_out:
out:	pop	{r1}
	str	r5, [r1, #ST_PC]
	str	r7, [r1, #ST_STEPS]
	pop	{r4-r11, pc}
	.ltorg

/* CF as get_CF has it, into cpu->flags (when pending); r1 r3 clobbered,
 * lr the return.  An op it doesn't know: the C loop's */
cfsync:	ldr	r1, [r4, #OFF_CC_MASK]
	tst	r1, #CF
	it	eq
	bxeq	lr
	ldr	r3, [r4, #OFF_CC_OP]
	cmp	r3, #28
	bhi	cfout
	tbb	[pc, r3]
1:	.byte	(cf_adc - 1b) / 2, (cf_add - 1b) / 2, (cf_sbb - 1b) / 2, (cf_sub - 1b) / 2
	.byte	(cf_neg - 1b) / 2, (cf_neg - 1b) / 2, (cf_neg - 1b) / 2
	.byte	(cfout - 1b) / 2, (cfout - 1b) / 2, (cfout - 1b) / 2           /* DEC */
	.byte	(cfout - 1b) / 2, (cfout - 1b) / 2, (cfout - 1b) / 2           /* INC */
	.byte	(cfout - 1b) / 2, (cfout - 1b) / 2, (cf_imul - 1b) / 2         /* IMUL */
	.byte	(cfout - 1b) / 2, (cfout - 1b) / 2, (cf_mul - 1b) / 2          /* MUL */
	.byte	(cf_shift - 1b) / 2, (cf_shift - 1b) / 2, (cf_shift - 1b) / 2  /* SAR SHL SHR */
	.byte	(cfout - 1b) / 2, (cfout - 1b) / 2                             /* SHLD SHRD */
	.byte	(cf_zero - 1b) / 2, (cf_zero - 1b) / 2                         /* BSF BSR */
	.byte	(cf_zero - 1b) / 2, (cf_zero - 1b) / 2, (cf_zero - 1b) / 2     /* AND OR XOR */
	.align	1
cfout:	b	out
cf_adc:	ldr	r1, [r4, #OFF_CC_DST]
	ldr	r3, [r4, #OFF_CC_SRC2]
	cmp	r1, r3
	ite	ls
	movls	r1, #1
	movhi	r1, #0
	b	cf_set
cf_add:	ldr	r1, [r4, #OFF_CC_DST]
	ldr	r3, [r4, #OFF_CC_SRC2]
	b	2f
cf_sbb:	ldr	r1, [r4, #OFF_CC_SRC1]
	ldr	r3, [r4, #OFF_CC_SRC2]
	cmp	r1, r3
	ite	ls
	movls	r1, #1
	movhi	r1, #0
	b	cf_set
cf_sub:	ldr	r1, [r4, #OFF_CC_SRC1]
	ldr	r3, [r4, #OFF_CC_SRC2]
2:	cmp	r1, r3
	ite	lo
	movlo	r1, #1
	movhs	r1, #0
	b	cf_set
cf_neg:	ldr	r1, [r4, #OFF_CC_DST]
	cmp	r1, #0
	it	ne
	movne	r1, #1
	b	cf_set
cf_imul: ldr	r1, [r4, #OFF_CC_DST]
	ldr	r3, [r4, #OFF_CC_DST2]
	cmp	r3, r1, asr #31
	ite	ne
	movne	r1, #1
	moveq	r1, #0
	b	cf_set
cf_mul:	ldr	r1, [r4, #OFF_CC_DST2]
	cmp	r1, #0
	it	ne
	movne	r1, #1
	b	cf_set
cf_shift: ldr	r1, [r4, #OFF_CC_DST2]
	and	r1, r1, #1
	b	cf_set
cf_zero: movs	r1, #0
cf_set:	ldr	r3, [r4, #OFF_FLAGS]
	bic	r3, r3, #CF
	orr	r3, r3, r1
	str	r3, [r4, #OFF_FLAGS]
	bx	lr

/* ---- MOV, LEA */
h_89:	add	r2, r5, #1
	EA	20f
	TLB	r1, 4, TLB_WTAG, out
	ubfx	r3, r0, #3, #3
	ldr	r3, [r4, r3, lsl #2]
	str	r3, [r1]
	NEXT
20:	ubfx	r3, r0, #3, #3
	ldr	r3, [r4, r3, lsl #2]
	and	r12, r0, #7
	str	r3, [r4, r12, lsl #2]
	NEXT

h_8b:	add	r2, r5, #1
	EA	20f
	TLB	r1, 4, TLB_RTAG, out
	ldr	r1, [r1]
	b	21f
20:	and	r12, r0, #7
	ldr	r1, [r4, r12, lsl #2]
21:	ubfx	r3, r0, #3, #3
	str	r1, [r4, r3, lsl #2]
	NEXT

h_8d:	add	r2, r5, #1
	EA	out
	ubfx	r3, r0, #3, #3
	str	r1, [r4, r3, lsl #2]
	NEXT

h_c7:	ldrb	r0, [r5, #1]
	tst	r0, #0x38
	bne	out
	add	r2, r5, #1
	EA	20f
	TLB	r1, 4, TLB_WTAG, out
	ldr	r0, [r2], #4
	str	r0, [r1]
	NEXT
20:	and	r12, r0, #7
	ldr	r0, [r2], #4
	str	r0, [r4, r12, lsl #2]
	NEXT

h_b8:	ldr	r1, [r5, #1]
	and	r0, r0, #7
	str	r1, [r4, r0, lsl #2]
	add	r2, r5, #5
	NEXT

/* ---- TEST Ev,Gv */
h_85:	add	r2, r5, #1
	EA	20f
	TLB	r1, 4, TLB_RTAG, out
	ldr	r3, [r1]
	b	21f
20:	and	r12, r0, #7
	ldr	r3, [r4, r12, lsl #2]
21:	ubfx	r12, r0, #3, #3
	ldr	r12, [r4, r12, lsl #2]
	and	lr, r3, r12
	movs	r3, #CC_AND
	strd	r3, lr, [r4, #OFF_CC_OP]
	movw	r3, #M_LOGIC
	str	r3, [r4, #OFF_CC_MASK]
	NEXT

/* ---- ALU */
h_01:	ALU_EVGV add
h_09:	ALU_EVGV or
h_21:	ALU_EVGV and
h_29:	ALU_EVGV sub
h_31:	ALU_EVGV xor
h_39:	ALU_EVGV cmp
h_03:	ALU_GVEV add
h_0b:	ALU_GVEV or
h_23:	ALU_GVEV and
h_2b:	ALU_GVEV sub
h_33:	ALU_GVEV xor
h_3b:	ALU_GVEV cmp
h_81:	GRP1	1
h_83:	GRP1	0

/* ---- PUSH POP */
h_50:	ldr	r2, [r4, #OFF_ESP]
	subs	r2, #4
	mov	r1, r2
	TLB	r1, 4, TLB_WTAG, out
	and	r0, r0, #7
	ldr	r0, [r4, r0, lsl #2]
	str	r0, [r1]
	str	r2, [r4, #OFF_ESP]
	add	r2, r5, #1
	NEXT

h_58:	ldr	r1, [r4, #OFF_ESP]
	add	r2, r1, #4
	TLB	r1, 4, TLB_RTAG, out
	ldr	r1, [r1]
	str	r2, [r4, #OFF_ESP]
	and	r0, r0, #7
	str	r1, [r4, r0, lsl #2]
	add	r2, r5, #1
	NEXT

h_68:	ldr	r0, [r5, #1]
	add	lr, r5, #5
	b	20f
h_6a:	ldrsb	r0, [r5, #1]
	add	lr, r5, #2
20:	push	{lr}
	ldr	r2, [r4, #OFF_ESP]
	subs	r2, #4
	mov	r1, r2
	TLB	r1, 4, TLB_WTAG, 21f
	str	r0, [r1]
	str	r2, [r4, #OFF_ESP]
	pop	{r2}
	NEXT
21:	pop	{r2}
	b	out

/* ---- CALL RET JMP */
h_e8:	ldr	r0, [r5, #1]
	ldr	r2, [r4, #OFF_ESP]
	subs	r2, #4
	mov	r1, r2
	TLB	r1, 4, TLB_WTAG, out
	str	r2, [r4, #OFF_ESP]
	add	r2, r5, #5
	sub	r3, r2, r11
	str	r3, [r1]
	add	r2, r0
	JUMP

h_c3:	ldr	r1, [r4, #OFF_ESP]
	add	r2, r1, #4
	TLB	r1, 4, TLB_RTAG, out
	ldr	r1, [r1]
	str	r2, [r4, #OFF_ESP]
	add	r2, r11, r1
	JUMP

h_c2:	ldr	r1, [r4, #OFF_ESP]
	ldrh	r0, [r5, #1]
	add	r2, r1, #4
	add	r2, r0
	TLB	r1, 4, TLB_RTAG, out
	ldr	r1, [r1]
	str	r2, [r4, #OFF_ESP]
	add	r2, r11, r1
	JUMP

h_eb:	ldrsb	r0, [r5, #1]
	add	r2, r5, #2
	add	r2, r0
	JUMP

h_e9:	ldr	r0, [r5, #1]
	add	r2, r5, #5
	add	r2, r0
	JUMP

/* ---- Jcc rel8 */
h_72:	JREL	lo, vs, 2
h_73:	JREL	hs, vc, 2
h_74:	JZS	ZF, 1, 2
h_75:	JZS	ZF, 0, 2
h_76:	JREL	ls, eq, 2
h_77:	JREL	hi, ne, 2
h_78:	JZS	SF, 1, 2
h_79:	JZS	SF, 0, 2
h_7c:	JREL	lt, lt, 2
h_7d:	JREL	ge, ge, 2
h_7e:	JREL	le, le, 2
h_7f:	JREL	gt, gt, 2

/* ---- eAX,imm32 */
h_05:	ACC32	add
h_0d:	ACC32	or
h_25:	ACC32	and
h_2d:	ACC32	sub
h_35:	ACC32	xor
h_3d:	ACC32	cmp

/* ---- bytes: MOV TEST CMP, group 1 Eb,Ib */
h_88:	add	r2, r5, #1
	EA	20f
	TLB	r1, 1, TLB_WTAG, out
	b	21f
20:	and	r12, r0, #7
	R8	r1, r12
21:	ubfx	r12, r0, #3, #3
	R8	r3, r12
	ldrb	r3, [r3]
	strb	r3, [r1]
	NEXT

h_8a:	add	r2, r5, #1
	EA	20f
	TLB	r1, 1, TLB_RTAG, out
	b	21f
20:	and	r12, r0, #7
	R8	r1, r12
21:	ldrb	r1, [r1]
	ubfx	r12, r0, #3, #3
	R8	r3, r12
	strb	r1, [r3]
	NEXT

h_c6:	ldrb	r0, [r5, #1]
	tst	r0, #0x38
	bne	out
	add	r2, r5, #1
	EA	20f
	TLB	r1, 1, TLB_WTAG, out
	b	21f
20:	and	r12, r0, #7
	R8	r1, r12
21:	ldrb	r0, [r2], #1
	strb	r0, [r1]
	NEXT

h_84:	add	r2, r5, #1
	EA	20f
	TLB	r1, 1, TLB_RTAG, out
	b	21f
20:	and	r12, r0, #7
	R8	r1, r12
21:	ldrb	r3, [r1]
	ubfx	r12, r0, #3, #3
	R8	r1, r12
	ldrb	r12, [r1]
	and	lr, r3, r12
	sxtb	lr, lr
	movs	r3, #CC_AND
	strd	r3, lr, [r4, #OFF_CC_OP]
	movw	r3, #M_LOGIC
	str	r3, [r4, #OFF_CC_MASK]
	NEXT

h_38:	add	r2, r5, #1
	EA	20f
	TLB	r1, 1, TLB_RTAG, out
	b	21f
20:	and	r12, r0, #7
	R8	r1, r12
21:	ldrb	r3, [r1]
	ubfx	r12, r0, #3, #3
	R8	r1, r12
	ldrb	r12, [r1]
	OPCALC8	cmp
	NEXT

h_3a:	add	r2, r5, #1
	EA	20f
	TLB	r1, 1, TLB_RTAG, out
	b	21f
20:	and	r12, r0, #7
	R8	r1, r12
21:	ldrb	r12, [r1]
	ubfx	r3, r0, #3, #3
	R8	r1, r3
	ldrb	r3, [r1]
	OPCALC8	cmp
	NEXT

h_3c:	ldrb	r3, [r4]
	ldrb	r12, [r5, #1]
	add	r2, r5, #2
	OPCALC8	cmp
	NEXT

h_a8:	ldrb	r3, [r4]
	ldrb	r12, [r5, #1]
	add	r2, r5, #2
	and	lr, r3, r12
	sxtb	lr, lr
	movs	r3, #CC_AND
	strd	r3, lr, [r4, #OFF_CC_OP]
	movw	r3, #M_LOGIC
	str	r3, [r4, #OFF_CC_MASK]
	NEXT

h_80:	ldrb	r0, [r5, #1]
	ubfx	r3, r0, #3, #3
	sub	r3, #2
	cmp	r3, #1
	bls	out
	add	r2, r5, #1
	EA	20f
	ubfx	r3, r0, #3, #3
	cmp	r3, #7
	beq	22f
	TLB	r1, 1, TLB_WTAG, out
	b	23f
22:	TLB	r1, 1, TLB_RTAG, out
	b	23f
20:	and	r12, r0, #7
	R8	r1, r12
23:	ldrb	r3, [r1]
	ldrb	r12, [r2], #1
	ubfx	lr, r0, #3, #3
	tbb	[pc, lr]
24:	.byte	(40f - 24b) / 2, (41f - 24b) / 2, (40f - 24b) / 2, (40f - 24b) / 2
	.byte	(44f - 24b) / 2, (45f - 24b) / 2, (46f - 24b) / 2, (47f - 24b) / 2
	.align	1
40:	OPCALC8	add
	NEXT
41:	OPCALC8	or
	NEXT
44:	OPCALC8	and
	NEXT
45:	OPCALC8	sub
	NEXT
46:	OPCALC8	xor
	NEXT
47:	OPCALC8	cmp
	NEXT

/* ---- INC/DEC r32 */
h_40:	CFSYNC
	and	r12, r0, #7
	ldr	r1, [r4, r12, lsl #2]
	tst	r0, #8
	ittee	eq
	addeq	r1, #1
	moveq	r3, #CC_INC32
	subne	r1, #1
	movne	r3, #CC_DEC32
	str	r1, [r4, r12, lsl #2]
	strd	r3, r1, [r4, #OFF_CC_OP]
	movw	r3, #M_INCDEC
	str	r3, [r4, #OFF_CC_MASK]
	add	r2, r5, #1
	NEXT

/* ---- group 5: INC DEC CALL JMP PUSH Ev */
h_ff:	ldrb	r0, [r5, #1]
	ubfx	r3, r0, #3, #3
	tbb	[pc, r3]
24:	.byte	(30f - 24b) / 2, (30f - 24b) / 2, (32f - 24b) / 2, (39f - 24b) / 2
	.byte	(32f - 24b) / 2, (39f - 24b) / 2, (32f - 24b) / 2, (39f - 24b) / 2
	.align	1
39:	b	out
30:	CFSYNC                          /* INC DEC */
	ldrb	r0, [r5, #1]
	add	r2, r5, #1
	EA	20f
	TLB	r1, 4, TLB_WTAG, out
	b	21f
20:	and	r12, r0, #7
	add	r1, r4, r12, lsl #2
21:	ldr	r12, [r1]
	tst	r0, #8
	ittee	eq
	addeq	r12, #1
	moveq	r3, #CC_INC32
	subne	r12, #1
	movne	r3, #CC_DEC32
	str	r12, [r1]
	strd	r3, r12, [r4, #OFF_CC_OP]
	movw	r3, #M_INCDEC
	str	r3, [r4, #OFF_CC_MASK]
	NEXT
32:	add	r2, r5, #1                  /* CALL JMP PUSH: the operand */
	EA	20f
	TLB	r1, 4, TLB_RTAG, out
	ldr	r1, [r1]
	b	21f
20:	and	r12, r0, #7
	ldr	r1, [r4, r12, lsl #2]
21:	ubfx	r3, r0, #3, #3
	cmp	r3, #4
	bne	22f
	add	r2, r11, r1                 /* JMP */
	JUMP
22:	mov	r0, r1
	ldr	r1, [r4, #OFF_ESP]
	sub	r1, #4
	str	r1, [sp, #-8]!              /* (the new ESP: TLB clobbers r3 r12 lr) */
	TLB	r1, 4, TLB_WTAG, 23f
	ldr	r12, [sp], #8
	str	r12, [r4, #OFF_ESP]
	ldrb	r12, [r5, #1]
	ubfx	r12, r12, #3, #3
	cmp	r12, #2
	beq	24f
	str	r0, [r1]                    /* PUSH */
	NEXT
24:	sub	r3, r2, r11                 /* CALL */
	str	r3, [r1]
	add	r2, r11, r0
	JUMP
23:	add	sp, #8
	b	out

/* ---- shifts: SHL SHR SAR Ev by imm8 (C1), 1 (D1), CL (D3) */
h_c1:	ldrb	r0, [r5, #1]
	ubfx	r3, r0, #3, #3
	cmp	r3, #4
	beq	22f
	cmp	r3, #5
	beq	22f
	cmp	r3, #7
	bne	out
22:	add	r2, r5, #1
	EA	20f
	TLB	r1, 4, TLB_WTAG, out
	b	21f
20:	and	r12, r0, #7
	add	r1, r4, r12, lsl #2
21:	ldrb	r12, [r5]
	cmp	r12, #0xd1
	beq	23f
	cmp	r12, #0xd3
	beq	24f
	ldrb	r12, [r2], #1
	b	25f
23:	movs	r12, #1
	b	25f
24:	ldrb	r12, [r4, #4]
25:	ands	r12, r12, #0x1f
	beq	28f
	ldr	r3, [r1]
	ubfx	lr, r0, #3, #3
	cmp	lr, #5
	beq	26f
	bhi	27f
	rsb	lr, r12, #32                /* SHL */
	lsr	lr, r3, lr
	and	lr, lr, #1
	str	lr, [r4, #OFF_CC_DST2]
	lsl	r3, r3, r12
	movs	lr, #CC_SHL
	b	29f
26:	str	r3, [r4, #OFF_CC_SRC1]       /* SHR */
	sub	lr, r12, #1
	lsr	lr, r3, lr
	and	lr, lr, #1
	str	lr, [r4, #OFF_CC_DST2]
	lsr	r3, r3, r12
	movs	lr, #CC_SHR
	b	29f
27:	sub	lr, r12, #1                 /* SAR */
	asr	lr, r3, lr
	and	lr, lr, #1
	str	lr, [r4, #OFF_CC_DST2]
	asr	r3, r3, r12
	movs	lr, #CC_SAR
29:	strd	lr, r3, [r4, #OFF_CC_OP]
	movw	lr, #M_LOGIC
	str	lr, [r4, #OFF_CC_MASK]
	str	r3, [r1]
28:	NEXT

/* ---- IMUL Gv,Ev,imm */
h_69:	add	r2, r5, #1
	EA	20f
	TLB	r1, 4, TLB_RTAG, out
	ldr	r1, [r1]
	b	21f
20:	and	r12, r0, #7
	ldr	r1, [r4, r12, lsl #2]
21:	ldrb	r12, [r5]
	cmp	r12, #0x69
	ite	eq
	ldreq	r12, [r2], #4
	ldrsbne	r12, [r2], #1
	strd	r1, r12, [r4, #OFF_CC_SRC1]
	smull	r3, lr, r1, r12
	str	r3, [r4, #OFF_CC_DST]
	str	lr, [r4, #OFF_CC_DST2]
	movs	r1, #CC_IMUL32
	str	r1, [r4, #OFF_CC_OP]
	movw	r1, #M_ALL
	str	r1, [r4, #OFF_CC_MASK]
	ubfx	r12, r0, #3, #3
	str	r3, [r4, r12, lsl #2]
	NEXT

/* ---- LEAVE, MOV moffs32 */
h_c9:	ldr	r1, [r4, #20]               /* EBP */
	add	r2, r1, #4
	TLB	r1, 4, TLB_RTAG, out
	ldr	r1, [r1]
	str	r2, [r4, #OFF_ESP]
	str	r1, [r4, #20]
	add	r2, r5, #1
	NEXT

h_a1:	ldr	r1, [r5, #1]
	TLB	r1, 4, TLB_RTAG, out
	ldr	r1, [r1]
	str	r1, [r4]
	add	r2, r5, #5
	NEXT

h_a3:	ldr	r1, [r5, #1]
	TLB	r1, 4, TLB_WTAG, out
	ldr	r0, [r4]
	str	r0, [r1]
	add	r2, r5, #5
	NEXT

/* ---- 66: MOV Ew,Gw / Gw,Ew */
h_66:	ldrb	r1, [r5, #1]
	add	r2, r5, #2
	cmp	r1, #0x89
	beq	30f
	cmp	r1, #0x39
	beq	w_39
	cmp	r1, #0x3b
	beq	w_3b
	cmp	r1, #0x3d
	beq	w_3d
	cmp	r1, #0x83
	beq	w_83
	cmp	r1, #0x85
	beq	w_85
	cmp	r1, #0xc7
	beq	w_c7
	cmp	r1, #0x8b
	bne	out
	EA	20f                         /* 66 8B */
	TLB	r1, 2, TLB_RTAG, out
	ldrh	r1, [r1]
	b	21f
20:	and	r12, r0, #7
	ldrh	r1, [r4, r12, lsl #2]
21:	ubfx	r3, r0, #3, #3
	strh	r1, [r4, r3, lsl #2]
	NEXT
30:	EA	20f                         /* 66 89 */
	TLB	r1, 2, TLB_WTAG, out
	b	21f
20:	and	r12, r0, #7
	add	r1, r4, r12, lsl #2
21:	ubfx	r3, r0, #3, #3
	ldrh	r3, [r4, r3, lsl #2]
	strh	r3, [r1]
	NEXT

/* ---- MOV r8,imm8, TEST Eb,imm8 */
h_b0:	ldrb	r1, [r5, #1]
	and	r0, r0, #7
	R8	r3, r0
	strb	r1, [r3]
	add	r2, r5, #2
	NEXT

h_f6:	ldrb	r0, [r5, #1]
	tst	r0, #0x38
	bne	out
	add	r2, r5, #1
	EA	20f
	TLB	r1, 1, TLB_RTAG, out
	b	21f
20:	and	r12, r0, #7
	R8	r1, r12
21:	ldrb	r3, [r1]
	ldrb	r12, [r2], #1
	and	lr, r3, r12
	sxtb	lr, lr
	movs	r3, #CC_AND
	strd	r3, lr, [r4, #OFF_CC_OP]
	movw	r3, #M_LOGIC
	str	r3, [r4, #OFF_CC_MASK]
	NEXT

/* the 16-bit CMP's and TEST's lazy flags: x (r3), y (r12) */
.macro CMP16
	sxth	r3, r3
	sxth	r12, r12
	sub	lr, r3, r12
	sxth	lr, lr
	strd	r3, r12, [r4, #OFF_CC_SRC1]
	movs	r3, #CC_SUB
	strd	r3, lr, [r4, #OFF_CC_OP]
	movw	r3, #M_ALL
	str	r3, [r4, #OFF_CC_MASK]
.endm
.macro TEST16
	and	lr, r3, r12
	sxth	lr, lr
	movs	r3, #CC_AND
	strd	r3, lr, [r4, #OFF_CC_OP]
	movw	r3, #M_LOGIC
	str	r3, [r4, #OFF_CC_MASK]
.endm

w_39:	EA	20f                         /* 66 39: CMP Ew,Gw */
	TLB	r1, 2, TLB_RTAG, out
	ldrh	r3, [r1]
	b	21f
20:	and	r12, r0, #7
	ldrh	r3, [r4, r12, lsl #2]
21:	ubfx	r12, r0, #3, #3
	ldrh	r12, [r4, r12, lsl #2]
	CMP16
	NEXT
w_3b:	EA	20f                         /* 66 3B: CMP Gw,Ew */
	TLB	r1, 2, TLB_RTAG, out
	ldrh	r12, [r1]
	b	21f
20:	and	r12, r0, #7
	ldrh	r12, [r4, r12, lsl #2]
21:	ubfx	r3, r0, #3, #3
	ldrh	r3, [r4, r3, lsl #2]
	CMP16
	NEXT
w_3d:	ldrh	r3, [r4]                    /* 66 3D: CMP AX,Iw */
	ldrh	r12, [r2], #2
	CMP16
	NEXT
w_83:	ldrb	r0, [r2]                    /* 66 83 /7: CMP Ew,Ib */
	and	r0, r0, #0x38
	cmp	r0, #0x38
	bne	out
	EA	20f
	TLB	r1, 2, TLB_RTAG, out
	ldrh	r3, [r1]
	b	21f
20:	and	r12, r0, #7
	ldrh	r3, [r4, r12, lsl #2]
21:	ldrsb	r12, [r2], #1
	CMP16
	NEXT
w_85:	EA	20f                         /* 66 85: TEST Ew,Gw */
	TLB	r1, 2, TLB_RTAG, out
	ldrh	r3, [r1]
	b	21f
20:	and	r12, r0, #7
	ldrh	r3, [r4, r12, lsl #2]
21:	ubfx	r12, r0, #3, #3
	ldrh	r12, [r4, r12, lsl #2]
	TEST16
	NEXT
w_c7:	ldrb	r0, [r2]                    /* 66 C7 /0: MOV Ew,Iw */
	tst	r0, #0x38
	bne	out
	EA	20f
	TLB	r1, 2, TLB_WTAG, out
	b	21f
20:	and	r12, r0, #7
	add	r1, r4, r12, lsl #2
21:	ldrh	r0, [r2], #2
	strh	r0, [r1]
	NEXT

/* ---- 0F: Jcc rel32, MOVZX */
h_0f:	ldrb	r1, [r5, #1]
	sub	r3, r1, #0x80
	cmp	r3, #15
	bhi	20f
	ldr	r12, =fx_ktab0f
	ldr	pc, [r12, r3, lsl #2]
20:	sub	r3, r1, #0x90
	cmp	r3, #15
	bhi	21f
	ldr	r12, =fx_ktab0f + 64
	ldr	pc, [r12, r3, lsl #2]
21:	add	r2, r5, #2
	cmp	r1, #0xb6
	beq	h_movzx8
	cmp	r1, #0xb7
	beq	h_movzx16
	cmp	r1, #0xbe
	beq	h_movsx8
	cmp	r1, #0xbf
	beq	h_movsx16
	cmp	r1, #0xaf
	beq	h_imul2
	b	out
	.ltorg

h_movzx8:
	EA	20f
	TLB	r1, 1, TLB_RTAG, out
	ldrb	r1, [r1]
	b	21f
20:	and	r12, r0, #7
	and	r3, r12, #3
	add	r3, r4, r3, lsl #2
	lsr	r12, r12, #2
	ldrb	r1, [r3, r12]
21:	ubfx	r3, r0, #3, #3
	str	r1, [r4, r3, lsl #2]
	NEXT

h_movzx16:
	EA	20f
	TLB	r1, 2, TLB_RTAG, out
	ldrh	r1, [r1]
	b	21f
20:	and	r12, r0, #7
	ldrh	r1, [r4, r12, lsl #2]
21:	ubfx	r3, r0, #3, #3
	str	r1, [r4, r3, lsl #2]
	NEXT

h_movsx8:
	EA	20f
	TLB	r1, 1, TLB_RTAG, out
	ldrsb	r1, [r1]
	b	21f
20:	and	r12, r0, #7
	R8	r3, r12
	ldrsb	r1, [r3]
21:	ubfx	r3, r0, #3, #3
	str	r1, [r4, r3, lsl #2]
	NEXT

h_movsx16:
	EA	20f
	TLB	r1, 2, TLB_RTAG, out
	ldrsh	r1, [r1]
	b	21f
20:	and	r12, r0, #7
	ldrsh	r1, [r4, r12, lsl #2]
21:	ubfx	r3, r0, #3, #3
	str	r1, [r4, r3, lsl #2]
	NEXT

h_imul2:
	EA	20f
	TLB	r1, 4, TLB_RTAG, out
	ldr	r12, [r1]
	b	21f
20:	and	r12, r0, #7
	ldr	r12, [r4, r12, lsl #2]
21:	ubfx	r0, r0, #3, #3
	ldr	r1, [r4, r0, lsl #2]
	strd	r1, r12, [r4, #OFF_CC_SRC1]
	smull	r3, lr, r1, r12
	str	r3, [r4, #OFF_CC_DST]
	str	lr, [r4, #OFF_CC_DST2]
	movs	r1, #CC_IMUL32
	str	r1, [r4, #OFF_CC_OP]
	movw	r1, #M_ALL
	str	r1, [r4, #OFF_CC_MASK]
	str	r3, [r4, r0, lsl #2]
	NEXT

/* SETcc Eb: the byte's address first (r1), then the condition (r0) */
.macro SETDST
	add	r2, r5, #2
	EA	20f
	TLB	r1, 1, TLB_WTAG, out
	b	21f
20:	and	r12, r0, #7
	R8	r1, r12
21:
.endm
.macro SETZS bit, ltrue, lfalse, etrue, efalse
	SETDST
	ldr	r3, [r4, #OFF_CC_MASK]
	tst	r3, #\bit
	beq	22f
	ldr	r3, [r4, #OFF_CC_DST]
	cmp	r3, #0
	ite	\ltrue
	mov\ltrue	r0, #1
	mov\lfalse	r0, #0
	b	23f
22:	ldr	r3, [r4, #OFF_FLAGS]
	tst	r3, #\bit
	ite	\etrue
	mov\etrue	r0, #1
	mov\efalse	r0, #0
23:	strb	r0, [r1]
	NEXT
.endm
.macro SETREL subcc, subinv, logcc, loginv
	SETDST
	ldr	r3, [r4, #OFF_CC_MASK]
	movw	r12, #M_ALL
	cmp	r3, r12
	bne	30f
	ldr	r3, [r4, #OFF_CC_OP]
	cmp	r3, #CC_SUB
	bne	out
	ldr	r3, [r4, #OFF_CC_SRC1]
	ldr	r12, [r4, #OFF_CC_SRC2]
	cmp	r3, r12
	ite	\subcc
	mov\subcc	r0, #1
	mov\subinv	r0, #0
	b	33f
30:	movw	r12, #M_LOGIC
	cmp	r3, r12
	bne	out
	ldr	r3, [r4, #OFF_CC_OP]
	sub	r3, #CC_AND
	cmp	r3, #2
	bhi	out
	ldr	r3, [r4, #OFF_CC_DST]
	cmp	r3, #0
	ite	\logcc
	mov\logcc	r0, #1
	mov\loginv	r0, #0
33:	strb	r0, [r1]
	NEXT
.endm

set_92:	SETREL	lo, hs, vs, vc
set_93:	SETREL	hs, lo, vc, vs
set_94:	SETZS	ZF, eq, ne, ne, eq
set_95:	SETZS	ZF, ne, eq, eq, ne
set_96:	SETREL	ls, hi, eq, ne
set_97:	SETREL	hi, ls, ne, eq
set_98:	SETZS	SF, lt, ge, ne, eq
set_99:	SETZS	SF, ge, lt, eq, ne
set_9c:	SETREL	lt, ge, lt, ge
set_9d:	SETREL	ge, lt, ge, lt
set_9e:	SETREL	le, gt, le, gt
set_9f:	SETREL	gt, le, gt, le

j32_82:	JREL	lo, vs, 6
j32_83:	JREL	hs, vc, 6
j32_84:	JZS	ZF, 1, 6
j32_85:	JZS	ZF, 0, 6
j32_86:	JREL	ls, eq, 6
j32_87:	JREL	hi, ne, 6
j32_88:	JZS	SF, 1, 6
j32_89:	JZS	SF, 0, 6
j32_8c:	JREL	lt, lt, 6
j32_8d:	JREL	ge, ge, 6
j32_8e:	JREL	le, le, 6
j32_8f:	JREL	gt, gt, 6

	.size	fx_kernel, . - fx_kernel

/* ---- the tables (Thumb addresses) */
#if FX_SRAM
	.section .time_critical.fx_ktab, "a", %progbits
#else
	.section .rodata.fx_ktab, "a", %progbits
#endif
	.align	2
	.global	fx_ktab
#define H(x) .word x + 1
#define O H(out)
#define O8 O; O; O; O; O; O; O; O
fx_ktab:
/* 00 */	O; H(h_01); O; H(h_03); O; H(h_05); O; O; O; H(h_09); O; H(h_0b); O; H(h_0d); O; H(h_0f)
/* 10 */	O8; O8
/* 20 */	O; H(h_21); O; H(h_23); O; H(h_25); O; O; O; H(h_29); O; H(h_2b); O; H(h_2d); O; O
/* 30 */	O; H(h_31); O; H(h_33); O; H(h_35); O; O; H(h_38); H(h_39); H(h_3a); H(h_3b); H(h_3c); H(h_3d); O; O
/* 40 */	H(h_40); H(h_40); H(h_40); H(h_40); H(h_40); H(h_40); H(h_40); H(h_40)
		H(h_40); H(h_40); H(h_40); H(h_40); H(h_40); H(h_40); H(h_40); H(h_40)
/* 50 */	H(h_50); H(h_50); H(h_50); H(h_50); H(h_50); H(h_50); H(h_50); H(h_50)
		H(h_58); H(h_58); H(h_58); H(h_58); H(h_58); H(h_58); H(h_58); H(h_58)
/* 60 */	O; O; O; O; O; O; H(h_66); O; H(h_68); H(h_69); H(h_6a); H(h_69); O; O; O; O
/* 70 */	O; O; H(h_72); H(h_73); H(h_74); H(h_75); H(h_76); H(h_77)
		H(h_78); H(h_79); O; O; H(h_7c); H(h_7d); H(h_7e); H(h_7f)
/* 80 */	H(h_80); H(h_81); O; H(h_83); H(h_84); H(h_85); O; O; H(h_88); H(h_89); H(h_8a); H(h_8b); O; H(h_8d); O; O
/* 90 */	O8; O8
/* a0 */	O; H(h_a1); O; H(h_a3); O; O; O; O; H(h_a8); O; O; O; O; O; O; O
/* b0 */	H(h_b0); H(h_b0); H(h_b0); H(h_b0); H(h_b0); H(h_b0); H(h_b0); H(h_b0)
		H(h_b8); H(h_b8); H(h_b8); H(h_b8); H(h_b8); H(h_b8); H(h_b8); H(h_b8)
/* c0 */	O; H(h_c1); H(h_c2); H(h_c3); O; O; H(h_c6); H(h_c7); O; H(h_c9); O; O; O; O; O; O
/* d0 */	O; H(h_c1); O; H(h_c1); O; O; O; O; O8
/* e0 */	O8; H(h_e8); H(h_e9); O; H(h_eb); O; O; O; O
/* f0 */	O; O; O; O; O; O; H(h_f6); O; O; O; O; O; O; O; O; H(h_ff)
fx_ktab0f:
		O; O; H(j32_82); H(j32_83); H(j32_84); H(j32_85); H(j32_86); H(j32_87)
		H(j32_88); H(j32_89); O; O; H(j32_8c); H(j32_8d); H(j32_8e); H(j32_8f)
		O; O; H(set_92); H(set_93); H(set_94); H(set_95); H(set_96); H(set_97)
		H(set_98); H(set_99); O; O; H(set_9c); H(set_9d); H(set_9e); H(set_9f)
