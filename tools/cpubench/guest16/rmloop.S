# rmloop: a 16-bit real-mode workload for cpubench's bench16 (run at 2000:0000,
# DS = ES = 3000h, SS = 3800h): word loads and stores, ALU, a shift, a
# counted inner loop, a CALL/RET per outer round. HLT at the end.
	.code16
	.text
start:
	mov	$0x3000, %ax
	mov	%ax, %ds
	mov	%ax, %es
	mov	$0x3800, %ax
	mov	%ax, %ss
	mov	$0xfffe, %sp
	xor	%bx, %bx
	xor	%dx, %dx
	xor	%di, %di
	mov	$300, %cx
outer:
	push	%cx
	mov	$200, %cx
	xor	%si, %si
inner:
	mov	(%si), %ax
	add	%ax, %bx
	adc	$0, %dx
	xor	%bx, %ax
	mov	%ax, 2(%si)
	shl	$1, %ax
	inc	%si
	inc	%si
	and	$0x3ff, %si
	cmp	$0x55, %al
	jne	1f
	inc	%di
1:	loop	inner
	call	mix
	pop	%cx
	loop	outer
	hlt
mix:
	push	%ax
	mov	%bx, %ax
	sub	%dx, %ax
	sar	$3, %ax
	or	%ax, %di
	test	$1, %di
	jz	2f
	neg	%di
2:	pop	%ax
	ret
