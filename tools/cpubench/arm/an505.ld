/* cpubench on qemu-system-arm -M mps2-an505: code and data in SSRAM1 (secure
 * alias, 4 MB); the guest's memory in PSRAM at 0x80000000 (bench.c). */
MEMORY
{
	RAM (rwx) : ORIGIN = 0x10000000, LENGTH = 4M
}
ENTRY(reset)
SECTIONS
{
	.text : {
		KEEP(*(.vectors))
		*(.text*)
		*(.rodata*)
		. = ALIGN(4);
	} > RAM
	.ARM.exidx : { *(.ARM.exidx*) } > RAM
	.data : { *(.data*) . = ALIGN(4); } > RAM
	.bss (NOLOAD) : { __bss_start__ = .; *(.bss*) *(COMMON) . = ALIGN(8); __bss_end__ = .; } > RAM
	end = .;
	__stack_top = ORIGIN(RAM) + LENGTH(RAM);
}
