; MODE13.COM - VGA mode 13h: a palette ramp, some text, a key to finish
        org 100h
        mov ax, 0013h
        int 10h
        ; a grey-to-colour palette in the top 64 entries
        mov dx, 3C8h
        mov al, 64
        out dx, al
        inc dx
        xor cx, cx
.pal:   mov al, cl              ; red ramps up
        out dx, al
        mov al, 0               ; no green
        out dx, al
        mov al, 63
        sub al, cl              ; blue ramps down
        out dx, al
        inc cx
        cmp cx, 64
        jb .pal
        ; vertical bars: the 256 colours, then the ramp below
        mov ax, 0A000h
        mov es, ax
        xor di, di
        mov bx, 0
.row:   xor cx, cx
.col:   mov al, cl
        cmp bx, 100
        jb .s
        mov ax, cx
        shr ax, 2
        add al, 64
.s:     stosb
        inc cx
        cmp cx, 320
        jb .col
        inc bx
        cmp bx, 200
        jb .row
        ; a line of text from the BIOS, in graphics
        mov ah, 02h
        xor bh, bh
        mov dx, 0B05h           ; row 11, column 5
        int 10h
        mov si, text
.t:     lodsb
        or al, al
        jz .k
        mov ah, 0Eh
        mov bl, 15
        int 10h
        jmp .t
.k:     xor ah, ah
        int 16h
        mov ax, 0003h
        int 10h
        mov dx, done
        mov ah, 09h
        int 21h
        mov ax, 4C00h
        int 21h
text    db 'Mode 13h on bdos!', 0
done    db 'MODE13 done', 13, 10, '$'
