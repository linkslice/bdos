; BIOSAPI.COM - corners of the BIOS and mouse driver that programs lean on,
; each checked against what a real PC answers. Prints one line:
;   BIOSAPI: stuff ok full ok vga ok mouse ok|none hook <key> arrow <code>
; The test types "a", "b" and then the Up arrow at the end: a hook on
; INT 15h AH=4Fh swallows the "a" (as keyboard-remapping programs do), and
; INT 21h AH=07h must give the arrow as 0 then its scan code 48h, not E0h.
        org 100h
        mov dx, hello
        call print

        ; INT 16h AH=05h: stuff a key, read it back; a full buffer says AL=1
        mov ah, 05h
        mov cx, 1E58h           ; 'X'
        int 16h
        cmp al, 0
        jne .stuffbad
        mov ah, 00h
        int 16h
        cmp al, 'X'
        jne .stuffbad
        mov dx, s_stuff
        call print
        mov cx, 20              ; the buffer holds 15
.fill:  push cx
        mov ah, 05h
        mov cx, 1E59h
        int 16h
        pop cx
        cmp al, 1
        je .full
        loop .fill
        mov dx, s_bad
        call print
        jmp .flush
.full:  mov dx, s_full
        call print
.flush: mov ah, 01h             ; empty it again
        int 16h
        jz .vga
        mov ah, 00h
        int 16h
        jmp .flush
.stuffbad:
        mov dx, s_bad
        call print

        ; INT 10h AH=1Bh: the state of the display (VGA only)
.vga:   push ds
        pop es
        mov di, state
        mov ah, 1Bh
        xor bx, bx
        int 10h
        cmp al, 1Bh
        jne .vgabad
        cmp byte [state + 4], 3         ; mode
        jne .vgabad
        cmp word [state + 5], 80        ; columns
        jne .vgabad
        cmp byte [state + 22h], 25      ; rows
        jne .vgabad
        cmp byte [state + 25h], 8       ; VGA, colour
        jne .vgabad
        les bx, [state]                 ; the static table: modes 0-7 there
        cmp byte [es:bx], 0FFh
        jne .vgabad
        mov dx, s_vga
        call print
        jmp .mouse
.vgabad:
        mov dx, s_vgabad
        call print

        ; INT 33h: sensitivity read back, the range, disable/enable answers
.mouse: xor ax, ax
        int 33h
        cmp ax, 0FFFFh
        je .havem
        mov dx, s_nomouse
        call print
        jmp .hook
.havem: mov ax, 1Ah
        mov bx, 25
        mov cx, 30
        mov dx, 40
        int 33h
        mov ax, 1Bh
        int 33h
        cmp bx, 25
        jne .mbad
        cmp cx, 30
        jne .mbad
        cmp dx, 40
        jne .mbad
        mov ax, 7
        mov cx, 10
        mov dx, 300
        int 33h
        mov ax, 8
        mov cx, 20
        mov dx, 150
        int 33h
        mov ax, 31h
        int 33h
        cmp ax, 10
        jne .mbad
        cmp bx, 20
        jne .mbad
        cmp cx, 300
        jne .mbad
        cmp dx, 150
        jne .mbad
        push ds                 ; a cursor shape of our own: accepted
        pop es
        mov ax, 9
        mov bx, 7
        mov cx, 7
        mov dx, cursor
        int 33h
        mov ax, 0Fh
        mov cx, 16
        mov dx, 32
        int 33h
        mov ax, 1Fh
        int 33h
        cmp ax, 1Fh
        jne .mbad
        mov ax, 20h
        int 33h
        cmp ax, 0FFFFh
        jne .mbad
        mov ax, 21h             ; the software reset: still there
        int 33h
        cmp ax, 0FFFFh
        jne .mbad
        mov ax, 1Bh             ; and the sensitivity back to as it was
        int 33h
        cmp bx, 50
        jne .mbad
        mov dx, s_mouse
        call print
        jmp .hook
.mbad:  mov dx, s_mbad
        call print

        ; INT 15h AH=4Fh: the keyboard intercept sees each scan code first
.hook:  mov ax, 3515h
        int 21h
        mov [old15], bx
        mov [old15 + 2], es
        mov dx, hook15
        mov ax, 2515h
        int 21h
        mov dx, s_ready         ; the test types now
        call print
        mov ah, 00h
        int 16h
        push ax
        push ds
        lds dx, [old15]
        mov ax, 2515h
        int 21h
        pop ds
        mov dx, s_hook
        call print
        pop ax
        mov dl, al
        mov ah, 02h
        int 21h

        ; INT 21h AH=07h: the grey Up arrow is 0, then 48h
        mov ah, 07h
        int 21h
        mov [arrow], al
        mov ah, 07h
        int 21h
        mov [arrow + 1], al
        mov dx, s_arrow
        call print
        movzx eax, word [arrow]
        xchg al, ah
        call hex32
        call crlf
        mov ax, 4C00h
        int 21h

; the 'a' key's make code is swallowed: CF clear says "drop it"
hook15: cmp ah, 4Fh
        jne .chain
        cmp al, 1Eh
        jne .chain
        push bp
        mov bp, sp
        and word [bp + 6], 0FFFEh       ; CF off in the flags IRET restores
        pop bp
        iret
.chain: jmp far [cs:old15]

%include "dos/lib.inc"

hello   db "BIOSAPI:$"
s_stuff db " stuff ok$"
s_full  db " full ok$"
s_bad   db " stuff BAD$"
s_vga   db " vga ok$"
s_vgabad db " vga BAD$"
s_mouse db " mouse ok$"
s_mbad  db " mouse BAD$"
s_nomouse db " mouse none$"
s_ready db " ready$"
s_hook  db " hook $"
s_arrow db " arrow $"
old15   dd 0
arrow   dw 0
cursor  times 16 dw 0FFFFh
        times 16 dw 0
state   times 64 db 0
