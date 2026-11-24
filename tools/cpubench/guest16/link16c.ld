SECTIONS { . = 0; .text : { *(.text.start) *(.text*) *(.rodata*) *(.data*) *(.bss*) *(COMMON) } }
