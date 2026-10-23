; lib.inc - small helpers for the DOS test programs (16-bit, .COM)

; print the $-terminated string at DX
print:  push ax
        mov ah, 09h
        int 21h
        pop ax
        ret

; print EAX in hex, eight digits
hex32:  push eax
        push cx
        push dx
        mov cx, 8
.h:     rol eax, 4
        push eax
        and al, 0Fh
        add al, '0'
        cmp al, '9'
        jbe .d
        add al, 7
.d:     mov dl, al
        mov ah, 02h
        int 21h
        pop eax
        loop .h
        pop dx
        pop cx
        pop eax
        ret

; print EAX in decimal
dec32:  push eax
        push ebx
        push ecx
        push edx
        mov ebx, 10
        xor ecx, ecx
.div:   xor edx, edx
        div ebx
        push dx
        inc cx
        or eax, eax
        jnz .div
.out:   pop dx
        add dl, '0'
        mov ah, 02h
        int 21h
        loop .out
        pop edx
        pop ecx
        pop ebx
        pop eax
        ret

crlf:   push ax
        push dx
        mov ah, 02h
        mov dl, 13
        int 21h
        mov dl, 10
        int 21h
        pop dx
        pop ax
        ret

; wait for a key (INT 16h), result in AX
getkey: mov ah, 00h
        int 16h
        ret
