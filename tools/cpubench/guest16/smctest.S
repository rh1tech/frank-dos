# smctest: self-modifying code against the JIT's chunks: an immediate the
# loop patches each round, an instruction patched later in its own block,
# and data stores into the code's page (outside the code's chunks).
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
	xor	%bx, %bx
	mov	$3000, %cx
l1:
patch:	mov	$0x1234, %ax
	add	%ax, %dx
	incw	%cs:patch+1
	mov	%dx, %cs:data
	add	%cs:data, %bx
	# the same block: the INC below becomes a DEC (48+r) every 4th round
	mov	%cx, %ax
	and	$3, %ax
	mov	$0x43, %al
	jnz	2f
	mov	$0x4b, %al
2:	mov	%al, %cs:site
	xor	%dx, %bx
site:	inc	%bx
	rol	$1, %bx
	loop	l1
	mov	%bx, 0x10
	mov	%dx, 0x12
	hlt
	.balign	512
data:	.word	0
