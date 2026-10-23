; KEYHOLD.COM - a game's keyboard: mode 13h, INT 9 reading port 60h and
; acknowledging through port 61h the way id's games do, and a table of which
; keys are down. Holding a key paints a bar for as long as it's held; Esc
; (released) ends it and prints how many timer ticks each key was down for,
; plus the mouse as the program saw it when the left button was last pressed,
; and how many times its INT 33h event handler was called.
        org 100h
        mov ax, 3509h
        int 21h
        mov [old9], bx
        mov [old9+2], es
        mov dx, kbd
        mov ax, 2509h
        int 21h
        mov ax, 0013h
        int 10h
        xor ax, ax              ; a mouse, if there is one
        int 33h
        mov [hasmouse], ax
        ; a game's range: 0..319 across, 0..199 down
        mov ax, 7
        xor cx, cx
        mov dx, 319
        int 33h
        mov ax, 8
        xor cx, cx
        mov dx, 199
        int 33h
        ; an event handler for left presses, as games install one
        push es
        push cs
        pop es
        mov ax, 0Ch
        mov cx, 2
        mov dx, mhandler
        int 33h
        pop es
        mov ax, 0A000h
        mov es, ax
.loop:  hlt
        ; the mouse, polled as a game would
        cmp word [hasmouse], 0
        je .nom
        mov ax, 3
        int 33h
        test bx, 1
        jz .nom
        mov [mx], cx
        mov [my], dx
.nom:   ; one bar per key held: count ticks while down
        mov bx, 2
.keys:  cmp byte [down + bx], 0
        je .next
        push ds
        xor si, si
        mov ds, si
        mov ax, [046Ch]
        pop ds
        cmp ax, [lasttick + bx]
        je .next
        mov [lasttick + bx], ax
        inc word [held + bx]
        ; draw: a row per key
        push bx
        mov ax, bx
        and ax, 0x3F
        mov dx, 320
        mul dx
        mov di, ax
        mov cx, 100
        mov al, 40
        rep stosb
        pop bx
.next:  inc bx
        inc bx
        cmp bx, 256
        jb .keys
        cmp byte [quit], 0
        je .loop
        ; done
        push ds
        lds dx, [old9]
        mov ax, 2509h
        int 21h
        pop ds
        mov ax, 0003h
        int 10h
        mov dx, msg
        call print
        mov bx, 2
.out:   mov ax, [held + bx]
        or ax, ax
        jz .skip
        mov ax, bx
        shr ax, 1
        call hex8
        mov dl, '='
        mov ah, 02h
        int 21h
        movzx eax, word [held + bx]
        call dec32
        mov dl, ' '
        mov ah, 02h
        int 21h
.skip:  inc bx
        inc bx
        cmp bx, 256
        jb .out
        mov dx, mmsg
        call print
        movzx eax, word [mx]
        call dec32
        mov dl, ','
        mov ah, 02h
        int 21h
        movzx eax, word [my]
        call dec32
        mov dx, emsg
        call print
        movzx eax, word [events]
        call dec32
        call crlf
        mov ax, 4C00h
        int 21h

; the mouse driver calls this (far) on a left press; it may use the stack
mhandler:
        push ax
        push ds
        push cs
        pop ds
        inc word [events]
        pop ds
        pop ax
        retf

kbd:    push ax
        push bx
        in al, 60h
        mov ah, al
        in al, 61h              ; the XT acknowledge
        or al, 80h
        out 61h, al
        and al, 7Fh
        out 61h, al
        mov al, ah
        cmp al, 0E0h
        je .eoi
        xor bx, bx
        mov bl, al
        and bl, 7Fh
        shl bx, 1               ; word index: the table is held[] / down[] by 2s
        test al, 80h
        jnz .up
        mov byte [cs:down + bx], 1
        jmp .eoi
.up:    mov byte [cs:down + bx], 0
        cmp al, 81h
        jne .eoi
        mov byte [cs:quit], 1
.eoi:   mov al, 20h
        out 20h, al
        pop bx
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

msg     db 'KEYHOLD ticks: $'
mmsg    db 13, 10, 'KEYHOLD mouse: $'
emsg    db ', events $'
events  dw 0
old9    dd 0
quit    db 0
hasmouse dw 0
mx      dw 0
my      dw 0
down    times 256 db 0
held    times 256 db 0
lasttick times 256 db 0
