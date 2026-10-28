; TURN.COM - a game that turns with the mouse, as Doom does: mode 13h, the
; pointer hidden, and every frame it asks the driver how far the mouse moved
; (INT 33h 0Bh) rather than where it is. A key ends it, and it prints the
; motion it saw in all:  TURN: mickeys <x> <y>
        org 100h
        mov ax, 0013h
        int 10h
        xor ax, ax
        int 33h
        cmp ax, 0FFFFh
        jne .done
.loop:  hlt
        mov ax, 0Bh
        int 33h
        movsx ecx, cx
        movsx edx, dx
        add [totx], ecx
        add [toty], edx
        mov ah, 01h
        int 16h
        jz .loop
        mov ah, 00h
        int 16h
.done:  mov ax, 0003h
        int 10h
        mov dx, msg
        call print
        mov eax, [totx]
        call sdec
        mov dl, ' '
        mov ah, 02h
        int 21h
        mov eax, [toty]
        call sdec
        call crlf
        mov ax, 4C00h
        int 21h

; EAX as a signed decimal
sdec:   or eax, eax
        jns .pos
        push eax
        mov dl, '-'
        mov ah, 02h
        int 21h
        pop eax
        neg eax
.pos:   jmp dec32

%include "dos/lib.inc"

msg     db "TURN: mickeys $"
totx    dd 0
toty    dd 0
