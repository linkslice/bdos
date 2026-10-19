; BOX.COM - paint straight into text video memory at B800:0000
        org 100h
        mov ax, 0B800h
        mov es, ax
        ; clear the screen with blue
        xor di, di
        mov ax, 1F20h
        mov cx, 80*25
        rep stosw
        ; a box from (20,8) to (59,14) in cyan on black
        mov bx, 8
.row:   mov ax, bx
        mov dx, 160
        mul dx
        add ax, 40              ; column 20 * 2
        mov di, ax
        mov cx, 40
        mov ax, 3020h
        rep stosw
        inc bx
        cmp bx, 15
        jne .row
        ; the message, character by character with an attribute
        mov si, text
        mov di, (11*80 + 26) * 2
.msg:   lodsb
        or al, al
        jz .done
        mov ah, 30h
        stosw
        jmp .msg
.done:  ; park the cursor below the box with BIOS and leave
        mov ah, 02h
        xor bh, bh
        mov dx, 1600h
        int 10h
        mov ax, 4C00h
        int 21h
text    db 'Written straight to B800:0000', 0
