; PLANAR.COM - VGA mode 12h (640x480, 16 colours, four planes): coloured
; stripes drawn with write mode 2, then a key
        org 100h
        mov ax, 0012h
        int 10h
        mov ax, 0A000h
        mov es, ax
        mov dx, 3CEh
        mov ax, 0205h           ; write mode 2
        out dx, ax
        xor di, di
        xor bx, bx              ; row
.row:   mov cx, 80              ; 80 bytes = 640 pixels
        mov ax, bx
        shr ax, 5               ; colour = row / 32
        and al, 15
        mov ah, al
.col:   mov al, [es:di]         ; load the latches
        mov al, ah
        stosb
        loop .col
        inc bx
        cmp bx, 480
        jb .row
        mov ax, 0005h           ; write mode 0 again
        out dx, ax
        xor ah, ah
        int 16h
        mov ax, 0003h
        int 10h
        mov dx, done
        mov ah, 09h
        int 21h
        mov ax, 4C00h
        int 21h
done    db 'PLANAR done', 13, 10, '$'
