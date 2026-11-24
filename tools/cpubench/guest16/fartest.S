# fartest: far CALL/RETF (also through a second CS:IP alias of the same
# code, and RETF imm), 66h CALL/RET, LES/LDS, segment pushes and pops, in a
# loop: the JIT against the interpreter.
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
	xor	%di, %di
	movw	$0x1234, 0x20
	movw	$0x3000, 0x22
	push	%ds                     # INT 60h's vector: 2000:isr60
	xor	%ax, %ax
	mov	%ax, %ds
	movw	$isr60, 0x180
	movw	$0x2000, 0x182
	pop	%ds
	mov	$2000, %cx
1:
	lcall	$0x2000, $farsub
	lcall	$0x1ff0, $farsub + 0x100        # the same code, CS 1FF0h
	push	%cx
	pushw	$7
	lcall	$0x2000, $farsub2
	pop	%cx
	calll	nearsub32
	les	0x20, %bx
	add	%bx, %dx
	mov	%es, %ax
	add	%ax, %di
	lds	0x20, %si
	mov	$0x3000, %ax
	mov	%ax, %ds
	push	%es
	push	%ds
	pop	%es
	pop	%ax
	xor	%ax, %dx
	# indirect: CALL/JMP near and far through registers and memory, PUSH m, moffs
	movw	$nearsub16, 0x30
	movw	$farsub, 0x34
	movw	$0x2000, 0x36
	call	*0x30
	mov	$nearsub16, %bx
	call	*%bx
	lcall	*0x34
	es lcall *0x34
	pushw	0x20
	pop	%ax
	add	%ax, %di
	mov	0x22, %ax
	mov	%ax, 0x24
	add	%ax, %dx
	movw	$2f, 0x38
	jmp	*0x38
	inc	%dx
2:	int	$0x60
	push	%dx
	pop	0x26                    # POP Ev, 16-bit
	pushl	%ecx
	popl	0x28                    # and 32-bit
	add	0x26, %di
	add	0x28, %dx
	cli
	dec	%cx
	jnz	1b
	mov	%dx, 0x10
	mov	%di, 0x12
	hlt
farsub:
	add	%cx, %dx
	rol	$3, %dx
	lret
farsub2:
	mov	%sp, %bp
	add	4(%bp), %dx
	lret	$2
nearsub32:
	inc	%di
	retl
nearsub16:
	add	$3, %di
	ret
isr60:
	inc	%dx
	rol	$1, %di
	iret
