; FRANK DOS — the BIOS ROM (segment F000h), 64 KB.
;
; The guest's side of the native BIOS: every vector the BIOS owns points
; here, at a few bytes of x86 around a trap (0F FF <id>) into the ARM code
; (src/hle). What has to stay guest code is guest code: the IRET, EOIs,
; the INT 1Ch call from the timer, INT 15h/4Fh from the keyboard, the jump
; to the boot sector. A program that hooks a vector chains to these stubs
; as it would to a real BIOS.
;
; A native body that would block (INT 16h with no key, INT 15h/86h) makes
; the trap resume at its stub's wait loop: STI, HLT, back to the trap. The
; guest idles with interrupts on, so IRQs run, and nothing nests.
;
; Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
; SPDX-License-Identifier: MIT

        bits 16
        org 0
%include "rom_layout.inc"

%macro TRAP 1
        db 0x0F, 0xFF
        dw %1
%endmacro

; an INT whose whole body is native: trap, IRET; its wait loop right after
; (the native side resumes at trap + 5 to wait)
%macro HANDLER 1
%%top:  TRAP %1
        iret
        sti
        hlt
        jmp short %%top
%endmacro

%macro AT 1
        times (%1) - ($ - $$) db 0
%endmacro

; ---- the vector table POST installs (0: the vector is left alone) ---------
vectors:
%assign n 0
%rep 256
  %if n < 8
        dw int_iret                     ; exceptions / NMI
  %elif n == 0x08
        dw int08
  %elif n == 0x09
        dw int09
  %elif n >= 0x0A && n <= 0x0F
        dw irq_master
  %elif n == 0x10
        dw int10
  %elif n == 0x11
        dw int11
  %elif n == 0x12
        dw int12
  %elif n == 0x13
        dw int13
  %elif n == 0x14
        dw int14
  %elif n == 0x15
        dw int15
  %elif n == 0x16
        dw int16
  %elif n == 0x17
        dw int17
  %elif n == 0x18
        dw int18
  %elif n == 0x19
        dw int19
  %elif n == 0x1A
        dw int1a
  %elif n == 0x1D || n == 0x1E || n == 0x1F
        dw 0                            ; data pointers: set by POST
  %elif n == 0x40
        dw int13                        ; diskette services, as INT 13h moves them
  %elif n == 0x41 || n == 0x46 || n == 0x43
        dw 0
  %elif n == 0x70
        dw irq8
  %elif n == 0x71
        dw irq9
  %elif n == 0x74
        dw irq12
  %elif n >= 0x72 && n <= 0x77
        dw irq_slave
  %else
        dw int_iret
  %endif
  %assign n n + 1
%endrep

        AT ROM_MOUSE_FN
mouse_fn:
        dd 0

; ---- the stubs ----------------------------------------------------------------
        AT 0x1000
int_iret:
        iret

irq_master:                             ; IRQ 2-7 nobody took
        push ax
        mov al, 0x20
        out 0x20, al
        pop ax
        iret

irq_slave:                              ; IRQ 10-15 nobody took
        push ax
        mov al, 0x20
        out 0xA0, al
        out 0x20, al
        pop ax
        iret

irq8:                                   ; RTC: acknowledge (register C)
        push ax
        mov al, 0x0C
        out 0x70, al
        in al, 0x71
        mov al, 0x20
        out 0xA0, al
        out 0x20, al
        pop ax
        iret

irq12:                                  ; IRQ 12: a byte from the PS/2 mouse
        push ax
        push bx
        push cx
        push dx
        TRAP 0x74                       ; AL 1: a whole packet, CX status, BX X, DX Y
        test al, al
        jz .eoi
        push cx                         ; the handler's frame (INT 15h C207h):
        push bx                         ; status, X, Y, 0, then a far call
        push dx
        xor ax, ax
        push ax
        call far [cs:mouse_fn]
        add sp, 8
.eoi:   cli
        mov al, 0x20
        out 0xA0, al
        out 0x20, al
        pop dx
        pop cx
        pop bx
        pop ax
        iret

irq9:                                   ; IRQ 9 is the old IRQ 2
        push ax
        mov al, 0x20
        out 0xA0, al
        pop ax
        int 0x0A
        iret

int08:                                  ; IRQ 0: the tick count, then INT 1Ch
        TRAP 0x08
        int 0x1C
        push ax
        mov al, 0x20
        out 0x20, al
        pop ax
        iret

int09:                                  ; IRQ 1: a byte from the keyboard
        push ax
        in al, 0x60
        mov ah, 0x4F                    ; INT 15h/4Fh may take or change it
        stc
        int 0x15
        jnc .done                       ; CF clear: taken
        TRAP 0x09                       ; AL the byte; AH 1 on return: Ctrl+Break
        test ah, ah
        jz .done
        int 0x1B
.done:  cli
        mov al, 0x20
        out 0x20, al
        pop ax
        iret

int10:  HANDLER 0x10
int11:  HANDLER 0x11
int12:  HANDLER 0x12
int13:  HANDLER 0x13
int14:  HANDLER 0x14
int15:  HANDLER 0x15
int16:  HANDLER 0x16
int17:  HANDLER 0x17
int1a:  HANDLER 0x1A

int18:                                  ; no bootable disk
        TRAP 0x18
.halt:  sti
        hlt
        jmp short .halt

; ---- POST and the boot ------------------------------------------------------
; ---- the FreeDOS kernel's entries: INT n -> TRAP_DOS + n, then IRET -------------
        AT ROM_DOS_STUBS
%assign n 0
%rep 256
        TRAP TRAP_DOS + n
        iret
        db 0x90, 0x90, 0x90
%assign n n + 1
%endrep

        AT ROM_POST
post:
        cli
        TRAP TRAP_POST                  ; the machine set up natively; SS:SP 0030:0100
        sti
        int 0x19
        int 0x18

int19_body:
        TRAP 0x19                       ; the boot sector at 0000:7C00, DL its drive; AX 0 if so
        test ax, ax
        jnz .fail
        jmp 0x0000:0x7C00
.fail:  int 0x18

        AT 0xE6F2                       ; INT 19h's IBM address: 3 bytes to the table
int19:  jmp int19_body

        AT ROM_CONFIG_TABLE             ; INT 15h C0h
        dw 8                            ; bytes that follow
        db 0xFC                         ; model: AT
        db 0x01                         ; submodel
        db 0x00                         ; BIOS revision
        db 0x70                         ; 2nd PIC, RTC, INT 15h/4Fh
        db 0x40                         ; extended BIOS data area: no; non-8042 kbd: no
        db 0, 0, 0

        AT ROM_DPT                      ; 1.44 MB diskette parameters
        db 0xAF, 0x02, 0x25, 0x02, 0x12, 0x1B, 0xFF, 0x6C, 0xF6, 0x0F, 0x08

        AT ROM_FONT8X8_LO               ; filled by POST (font8x8, 0-127)

        AT ROM_CALLBACK
        TRAP TRAP_CALLBACK
        AT ROM_IRET
        iret
        AT ROM_IDLE
.idle:  sti
        hlt
        jmp short .idle
        AT ROM_INT24
        mov al, 3
        iret
        AT ROM_NLS
        retf

        AT ROM_RESET
        jmp ROM_SEG:post
        AT ROM_DATE
        db "11/24/26"
        AT ROM_MODEL
        db 0xFC, 0x00
