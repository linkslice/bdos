; MOUSE.COM - the INT 33h driver in mode 13h: shows the pointer and
; reports where the left button was pressed. Right button (or a key) ends.
        org 100h
        cpu 386
        mov ax, 0013h
        int 10h
        xor ax, ax
        int 33h
        cmp ax, 0FFFFh
        jne .none
        mov ax, 1
        int 33h
.loop:  mov ah, 01h
        int 16h
        jnz .end
        mov ax, 3
        int 33h
        test bx, 2
        jnz .end
        test bx, 1
        jz .loop
        mov [mx], cx
        mov [my], dx
        ; a white dot where it was clicked (X is 0-639 in mode 13h)
        shr cx, 1
        mov ax, 320
        mul dx
        add ax, cx
        mov di, ax
        mov ax, 0A000h
        mov es, ax
        mov byte [es:di], 15
        mov byte [clicked], 1
        jmp .loop
.end:   mov ax, 2
        int 33h
        mov ax, 0003h
        int 10h
        cmp byte [clicked], 0
        je .noclick
        mov dx, msg
        mov ah, 09h
        int 21h
        movzx eax, word [mx]
        call dec32
        mov dl, ','
        mov ah, 02h
        int 21h
        movzx eax, word [my]
        call dec32
        call crlf
.noclick:
        mov ax, 4C00h
        int 21h
.none:  mov ax, 0003h
        int 10h
        mov dx, nomouse
        mov ah, 09h
        int 21h
        mov ax, 4C01h
        int 21h

%include "dos/lib.inc"

msg     db 'MOUSE: clicked at $'
nomouse db 'MOUSE: no mouse driver', 13, 10, '$'
mx      dw 0
my      dw 0
clicked db 0
