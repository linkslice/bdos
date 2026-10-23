; FPU.COM - the x87: square roots, trig, integer conversion, the status word
        org 100h
        cpu 486
        finit
        ; sqrt(2) * 1000 -> 1414
        fld qword [two]
        fsqrt
        fmul qword [thousand]
        fistp dword [v1]
        ; sin(pi/6) * 1000 -> 500
        fldpi
        fdiv qword [six]
        fsin
        fmul qword [thousand]
        fistp dword [v2]
        ; 10 / 4 = 2.5, rounded to nearest even -> 2; then 3.5 -> 4
        fld qword [ten]
        fdiv qword [four]
        fistp dword [v3]
        fld qword [threehalf]
        frndint
        fistp dword [v4]
        ; a comparison through the status word: 3 > 2
        fld qword [three]
        fcomp qword [two]
        fnstsw ax
        sahf
        seta bl
        movzx eax, bl
        mov [v5], eax
        ; FSAVE / FRSTOR keep a value
        fld qword [ten]
        fnsave [state]
        fld1
        frstor [state]
        fistp dword [v6]        ; 10
        ; the control word the program set comes back
        mov word [cw], 0C7Fh    ; round toward zero
        fldcw [cw]
        fstcw [cw2]
        fld qword [threehalf]
        fistp dword [v7]        ; 3 (truncated)
        movzx eax, word [cw2]
        mov [v8], eax
        mov si, res
        mov cx, 8
.p:     mov eax, [si]
        call dec32
        mov dl, ' '
        mov ah, 02h
        int 21h
        add si, 4
        loop .p
        call crlf
        mov si, res
        mov di, expect
        mov cx, 8
.c:     mov eax, [si]
        cmp eax, [di]
        jne .bad
        add si, 4
        add di, 4
        loop .c
        mov dx, good
        call print
        mov ax, 4C00h
        int 21h
.bad:   mov dx, bad
        call print
        mov ax, 4C01h
        int 21h

%include "dos/lib.inc"

good    db 'FPU OK', 13, 10, '$'
bad     db 'FPU WRONG', 13, 10, '$'
two     dq 2.0
three   dq 3.0
four    dq 4.0
six     dq 6.0
ten     dq 10.0
thousand dq 1000.0
threehalf dq 3.5
cw      dw 0
cw2     dw 0
res:
v1      dd 0
v2      dd 0
v3      dd 0
v4      dd 0
v5      dd 0
v6      dd 0
v7      dd 0
v8      dd 0
expect  dd 1414, 500, 2, 4, 1, 10, 3, 0C7Fh
state   times 108 db 0
