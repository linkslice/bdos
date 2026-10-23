; CLICKS.COM - quick clicks a slow game would miss. An INT 33h event
; handler counts left presses and whether BX said the button was down;
; the main loop polls function 3 only once a second, like a busy game.
; A right click ends it.
        org 100h
        cpu 386
        mov ax, 0013h
        int 10h
        xor ax, ax
        int 33h
        mov ax, 0Ch                     ; left button pressed -> handler
        mov cx, 2
        mov dx, handler
        push cs
        pop es
        int 33h
.sec:   push ds
        push 40h
        pop ds
        mov ax, [6Ch]
        add ax, 18
.wait:  cmp [6Ch], ax                   ; a second of "drawing the level"
        jb .wait
        pop ds
        mov ax, 3
        int 33h
        test bx, 2                      ; the right button: done
        jnz .end
        test bx, 1
        jz .sec
        inc word [polled]
        jmp .sec
.end:
        xor ax, ax                      ; reset: no more handler
        int 33h
        mov ax, 0003h
        int 10h
        mov dx, msg1
        mov ah, 09h
        int 21h
        movzx eax, word [hdown]
        call dec32
        mov dx, msg2
        mov ah, 09h
        int 21h
        movzx eax, word [hcalls]
        call dec32
        mov dx, msg3
        mov ah, 09h
        int 21h
        movzx eax, word [polled]
        call dec32
        call crlf
        mov ax, 4C00h
        int 21h

handler:
        inc word [cs:hcalls]
        test bx, 1
        jz .up
        inc word [cs:hdown]
.up:    retf

%include "dos/lib.inc"

msg1    db 'CLICKS: handler saw the button down $'
msg2    db ' of $'
msg3    db ', polling saw $'
hcalls  dw 0
hdown   dw 0
polled  dw 0
