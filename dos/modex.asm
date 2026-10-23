; MODEX.COM - "mode X": 320x240, 256 colours, unchained. Diagonal stripes
; written plane by plane, then a key.
        org 100h
        mov ax, 0013h
        int 10h
        mov dx, 3C4h
        mov ax, 0604h           ; chain-4 off
        out dx, ax
        mov dx, 3D4h
        mov ax, 0E317h          ; byte mode
        out dx, ax
        mov ax, 0014h           ; doubleword mode off
        out dx, ax
        mov ax, 2C11h           ; unprotect
        out dx, ax
        mov ax, 0D06h           ; vertical total
        out dx, ax
        mov ax, 3E07h           ; overflow
        out dx, ax
        mov ax, 0EA10h          ; retrace start
        out dx, ax
        mov ax, 0AC11h          ; retrace end (and protect)
        out dx, ax
        mov ax, 0DF12h          ; display end: 480 lines
        out dx, ax
        mov ax, 0E715h
        out dx, ax
        mov ax, 0616h
        out dx, ax
        mov ax, 0A000h
        mov es, ax
        mov cl, 0               ; plane
.plane: mov dx, 3C4h
        mov al, 2
        mov ah, 1
        shl ah, cl
        out dx, ax
        xor di, di
        xor bx, bx              ; y
.row:   xor si, si              ; x / 4
.col:   mov ax, si
        shl ax, 2
        add al, cl              ; x
        add ax, bx              ; x + y
        stosb
        inc si
        cmp si, 80
        jb .col
        inc bx
        cmp bx, 240
        jb .row
        inc cl
        cmp cl, 4
        jb .plane
        xor ah, ah
        int 16h
        mov ax, 0003h
        int 10h
        mov dx, done
        mov ah, 09h
        int 21h
        mov ax, 4C00h
        int 21h
done    db 'MODEX done', 13, 10, '$'
