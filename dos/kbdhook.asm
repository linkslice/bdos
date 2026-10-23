; KBDHOOK.COM - hook INT 9 and print the raw scancodes from port 60h
; until Escape is released (scancode 81h).
        org 100h
        mov ax, 3509h
        int 21h
        mov [old9], bx
        mov [old9+2], es
        mov dx, kbd
        mov ax, 2509h
        int 21h
        mov dx, prompt
        call print
.loop:  hlt
        mov si, [tail]
        cmp si, [head]
        je .loop
        mov al, [buf + si]
        inc si
        and si, 63
        mov [tail], si
        push ax
        call hex8
        mov dl, ' '
        mov ah, 02h
        int 21h
        pop ax
        cmp al, 81h             ; Escape released
        jne .loop
        push ds
        lds dx, [old9]
        mov ax, 2509h
        int 21h
        pop ds
        call crlf
        mov ax, 4C00h
        int 21h

kbd:    push ax
        push si
        in al, 60h
        mov si, [cs:head]
        mov [cs:buf + si], al
        inc si
        and si, 63
        mov [cs:head], si
        mov al, 20h
        out 20h, al
        pop si
        pop ax
        iret

hex8:   push ax
        mov ah, al
        shr al, 4
        call .n
        mov al, ah
        and al, 0Fh
        call .n
        pop ax
        ret
.n:     push ax
        add al, '0'
        cmp al, '9'
        jbe .o
        add al, 7
.o:     mov dl, al
        mov ah, 02h
        int 21h
        pop ax
        ret

%include "dos/lib.inc"

prompt  db 'KBDHOOK: press keys, Esc to stop', 13, 10, '$'
old9    dd 0
head    dw 0
tail    dw 0
buf     times 64 db 0
