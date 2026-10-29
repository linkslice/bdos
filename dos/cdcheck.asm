; CDCHECK.COM - finds the CD drives the way DOS games do, through MSCDEX
; (INT 2Fh, AX=15xxh), and reads the first one's volume name off the disc.
;
;   MSCDEX 2.23: 1 drive(s) from D:
;   D: is a CD drive, volume BDOS_DISC1
;   media changed: 0 then 1
        org 100h
        mov ax, 1500h           ; installed? BX drives, CX the first
        xor bx, bx
        int 2Fh
        or bx, bx
        jnz .found
        mov dx, none
        mov ah, 09h
        int 21h
        mov ax, 4C01h
        int 21h
.found: mov [count], bl
        mov [drive], cl
        mov ax, 150Ch           ; version: BH.BL
        int 2Fh
        mov al, bh
        add al, '0'
        mov [ver], al
        mov al, bl
        aam                     ; 23 -> AH 2, AL 3
        add ax, 3030h
        mov [ver + 2], ah
        mov [ver + 3], al
        mov al, [count]
        add al, '0'
        mov [cnt], al
        mov al, [drive]
        add al, 'A'
        mov [from], al
        mov [let], al
        mov dx, head
        mov ah, 09h
        int 21h

        mov ax, 150Bh           ; is it? AX nonzero, BX ADADh
        xor ch, ch
        mov cl, [drive]
        int 2Fh
        cmp bx, 0ADADh
        jne .bad
        or ax, ax
        jz .bad

        mov ax, 1505h           ; the first volume descriptor, sector 16
        xor ch, ch
        mov cl, [drive]
        xor dx, dx
        mov bx, buf
        int 2Fh
        jc .bad
        mov si, buf + 40        ; its volume identifier, 32 characters
        mov di, vol
        mov cx, 32
        rep movsb
        mov byte [di], '$'
        mov dx, isa
        mov ah, 09h
        int 21h
        mov dx, vol
        int 21h
        mov dx, crlf
        int 21h

        call changed            ; media changed: 0 (don't know) the first time
        mov [mc1], al
        call changed            ; then 1 (no)
        mov [mc2], al
        mov dx, media
        mov ah, 09h
        int 21h
        mov ax, 4C00h
        int 21h
.bad:   mov dx, notcd
        mov ah, 09h
        int 21h
        mov ax, 4C02h
        int 21h

; IOCTL input 9, media changed, through a device request (1510h)
changed:
        mov byte [rq + 2], 3    ; IOCTL input
        mov word [rq + 14], cb
        mov [rq + 16], cs
        mov word [rq + 18], 2
        mov byte [cb], 9
        mov ax, 1510h
        xor ch, ch
        mov cl, [drive]
        mov bx, rq
        push cs
        pop es
        int 2Fh
        mov al, [cb + 1]
        cmp al, 0FFh
        jne .n
        mov al, '-' - '0'
.n:     add al, '0'
        ret

count   db 0
drive   db 0
head    db "MSCDEX "
ver     db "?.?? : "
cnt     db "? drive(s) from "
from    db "?:", 13, 10, "$"
isa     db "D: is a CD drive, volume "
let     equ isa
vol     times 33 db 0
crlf    db 13, 10, "$"
media   db "media changed: "
mc1     db "? then "
mc2     db "?", 13, 10, "$"
none    db "No CD drives (MSCDEX isn't there)", 13, 10, "$"
notcd   db "The CD drive doesn't answer as one", 13, 10, "$"
rq      times 26 db 0
cb      times 8 db 0
buf     times 2048 db 0
