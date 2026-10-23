; CPU486.COM - 386 and 486 instructions in real mode
        org 100h
        cpu 586
        mov dx, title
        call print
        ; 32-bit arithmetic: 123456789 * 7 / 3
        mov eax, 123456789
        mov ecx, 7
        mul ecx
        mov ecx, 3
        div ecx
        mov [v1], eax           ; 288065841
        ; MOVZX, MOVSX
        mov bl, 0F0h
        movzx eax, bl
        movsx edx, bl
        add eax, edx            ; 0F0h + 0FFFFFFF0h = 0E0h (carry out)
        mov [v2], eax
        ; BSF/BSR
        mov eax, 00010100h
        bsf ecx, eax            ; 8
        bsr edx, eax            ; 16
        shl edx, 8
        or ecx, edx
        mov [v3], ecx           ; 1008h
        ; SHLD/SHRD
        mov eax, 12345678h
        mov edx, 9ABCDEF0h
        shld eax, edx, 8        ; 3456789Ah
        mov [v4], eax
        ; IMUL v32, r/m32, imm
        mov ebx, -1000
        imul eax, ebx, 3000     ; -3000000
        mov [v5], eax
        ; BSWAP, XADD, CMPXCHG (486)
        mov eax, 11223344h
        bswap eax               ; 44332211h
        mov [v6], eax
        mov eax, 5
        mov ebx, 10
        xadd eax, ebx           ; eax 15, ebx 5
        add eax, ebx            ; 20
        mov [v7], eax
        mov eax, 20
        mov ecx, 99
        mov edx, 20
        cmpxchg edx, ecx        ; edx == eax: edx = 99
        mov [v8], edx
        ; SETcc, BT
        mov eax, 0Ah
        bt eax, 1
        setc bl                 ; 1
        bt eax, 2
        setc bh                 ; 0
        movzx eax, bx           ; 0001h
        mov [v9], eax
        ; 32-bit addressing with a scaled index
        mov esi, table
        mov ecx, 3
        mov eax, [esi + ecx*4]  ; 40
        mov [v10], eax
        ; CPUID: a 486 that has it
        mov eax, 0
        cpuid
        mov [vendor], ebx
        mov [vendor+4], edx
        mov [vendor+8], ecx
        ; print them
        mov si, results
        mov cx, 10
.p:     mov eax, [si]
        call hex32
        mov dl, ' '
        mov ah, 02h
        int 21h
        add si, 4
        loop .p
        call crlf
        mov dx, vendor
        call print
        call crlf
        ; check
        mov si, results
        mov di, expect
        mov cx, 10
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

title   db 'CPU486: ', '$'
good    db 'CPU486 OK', 13, 10, '$'
bad     db 'CPU486 WRONG', 13, 10, '$'
vendor  db '????????????', '$'
table   dd 10, 20, 30, 40
results:
v1      dd 0
v2      dd 0
v3      dd 0
v4      dd 0
v5      dd 0
v6      dd 0
v7      dd 0
v8      dd 0
v9      dd 0
v10     dd 0
expect  dd 288065841, 0E0h, 1008h, 3456789Ah, -3000000, 44332211h, 20, 99, 1, 40
