# jcctest: every Jcc after every flag-setting kind the JIT tracks (CMP, SUB,
# AND, TEST, XOR, OR, ADD, ADC, SBB, INC, DEC; words and bytes; with a MOV
# between), on pseudo-random and edge operands. Each branch taken or not
# goes into DX, so the state hash differs if any goes the other way.
	.code16
	.text
start:
	mov	$0x3000, %ax
	mov	%ax, %ds
	mov	%ax, %es
	mov	$0x3800, %ax
	mov	%ax, %ss
	mov	$0xfffe, %sp
	xor	%dx, %dx
	mov	$0x1234, %si
	mov	$24, %cx
outer:
	push	%cx
	mov	$40, %cx
inner:
	push	%cx
	# a = the LCG, or an edge value; b = an edge value, a, or the LCG turned
	mov	%si, %ax
	mov	$25173, %bx
	mul	%bx
	add	$13849, %ax
	mov	%ax, %si
	mov	%ax, %di
	mov	%ax, %bx
	and	$14, %bx
	mov	%cs:edges(%bx), %bp
	test	$0x30, %al
	jnz	1f
	mov	%cs:edges+2(%bx), %di
1:	test	$0x40, %al
	jz	2f
	mov	%di, %bp
	test	$0x80, %al
	jz	2f
	mov	%si, %bp
	ror	$5, %bp
2:
.irp op, cmp, sub, and, test, xor, or, add, adc, sbb
.irp jc, jo, jno, jb, jae, je, jne, jbe, ja, js, jns, jp, jnp, jl, jge, jle, jg
	mov	%di, %ax
	mov	%bp, %bx
	\op	%bx, %ax
	\jc	1f
	add	$0x1357, %dx
1:	rol	$1, %dx
	mov	%di, %ax
	mov	%bp, %bx
	\op	%bl, %al
	\jc	1f
	add	$0x2468, %dx
1:	rol	$1, %dx
.endr
.endr
.irp jc, jb, jae, je, jne, jbe, ja, js, jns, jl, jge, jle, jg
	mov	%di, %ax
	cmp	%bp, %ax
	mov	%ax, %cx
	mov	%cx, 0x100
	\jc	1f
	add	$0x0f0f, %dx
1:	rol	$1, %dx
	mov	%di, %ax
	inc	%ax
	\jc	1f
	add	$0x3c3c, %dx
1:	rol	$1, %dx
	mov	%di, %ax
	dec	%ax
	\jc	1f
	add	$0x5a5a, %dx
1:	rol	$1, %dx
	mov	%di, %ax
	inc	%ax
	dec	%ax
	inc	%ax
	and	%bp, %ax
	\jc	1f
	add	$0x7171, %dx
1:	rol	$1, %dx
.endr
	mov	%dx, %bx
	and	$0x7fe, %bx
	add	%dx, (%bx)
	pop	%cx
	dec	%cx
	jz	3f
	jmp	inner
3:	pop	%cx
	dec	%cx
	jz	4f
	jmp	outer
4:	hlt
edges:
	.word	0, 1, 0x7fff, 0x8000, 0xffff, 0x80, 0x7f, 0xff, 0
