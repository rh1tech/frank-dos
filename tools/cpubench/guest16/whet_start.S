# whet's entry (bench16 at 2000:0000): the tiny model, a stack at the top of
# the segment, whet_main, its checksum in EAX, HLT.
	.code16gcc
	.section .text.start
	.global _start
_start:
	mov	%cs, %ax
	mov	%ax, %ds
	mov	%ax, %es
	mov	%ax, %ss
	mov	$0xfff0, %esp
	calll	whet_main
	hlt
