; PRIMES.COM - count primes below 10000 by trial division
        org 100h
        xor bp, bp              ; count
        mov bx, 2
.cand:  cmp bx, 10000
        jae .print
        mov cx, 2
.try:   mov ax, cx
        mul cx
        cmp ax, bx
        ja .prime               ; cx*cx > bx: prime
        mov ax, bx
        xor dx, dx
        div cx
        or dx, dx
        jz .notprime
        inc cx
        jmp .try
.prime: inc bp
.notprime:
        inc bx
        jmp .cand
.print: mov ax, bp
        call printnum
        mov dx, msg
        mov ah, 09h
        int 21h
        ret                     ; RET to PSP:0 -> INT 20h
printnum:                       ; AX in decimal
        mov cx, 0
        mov bx, 10
.div:   xor dx, dx
        div bx
        push dx
        inc cx
        or ax, ax
        jnz .div
.out:   pop dx
        add dl, '0'
        mov ah, 02h
        int 21h
        loop .out
        ret
msg     db ' primes below 10000', 13, 10, '$'
