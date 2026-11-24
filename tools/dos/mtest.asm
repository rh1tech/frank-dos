; FRANK DOS bench: the INT 33h mouse, as a program sees it.
; Resets the driver (AX=0), then prints "x y buttons" whenever they change,
; until a key is pressed.  nasm -f bin -o MTEST.COM mtest.asm
        org 0x100
        xor ax, ax
        int 0x33
        cmp ax, 0xFFFF
        je .ok
        mov dx, nodrv
        mov ah, 9
        int 0x21
        ret
.ok:    mov dx, hello
        mov ah, 9
        int 0x21
.loop:  mov ax, 3
        int 0x33                        ; CX x, DX y, BX buttons
        cmp cx, [lx]
        jne .show
        cmp dx, [ly]
        jne .show
        cmp bx, [lb]
        je .key
.show:  mov [lx], cx
        mov [ly], dx
        mov [lb], bx
        mov ax, cx
        call num
        mov ax, [ly]
        call num
        mov ax, [lb]
        call num
        mov dl, 13
        mov ah, 2
        int 0x21
        mov dl, 10
        int 0x21
.key:   mov ah, 1
        int 0x16
        jz .loop
        xor ah, ah
        int 0x16
        push ax
        mov dx, keymsg
        mov ah, 9
        int 0x21
        pop ax
        call num
        ret

num:    ; AX as decimal, then a space
        mov bx, 10
        xor cx, cx
.d:     xor dx, dx
        div bx
        push dx
        inc cx
        test ax, ax
        jnz .d
.p:     pop dx
        add dl, '0'
        mov ah, 2
        int 0x21
        loop .p
        mov dl, ' '
        mov ah, 2
        int 0x21
        ret

lx      dw 0xFFFF
ly      dw 0xFFFF
lb      dw 0xFFFF
hello   db 'mouse ok: x y buttons', 13, 10, '$'
nodrv   db 'no mouse driver', 13, 10, '$'
keymsg  db 'key $'
