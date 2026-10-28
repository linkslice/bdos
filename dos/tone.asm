; TONE.COM - 1000 Hz on the PC speaker for two seconds, the way programs
; beep: the PIT's channel 2 as a square wave, gated through port 61h.
        org 100h
        mov al, 0B6h            ; channel 2, lobyte/hibyte, mode 3
        out 43h, al
        mov ax, 1193            ; 1193182 / 1193 = 1000 Hz
        out 42h, al
        mov al, ah
        out 42h, al
        in al, 61h
        or al, 3                ; gate on, speaker on
        out 61h, al
        xor ah, ah              ; two seconds of timer ticks
        int 1Ah
        mov bx, dx
.wait:  hlt
        xor ah, ah
        int 1Ah
        mov ax, dx
        sub ax, bx
        cmp ax, 36
        jb .wait
        in al, 61h
        and al, 0FCh            ; off
        out 61h, al
        mov dx, msg
        mov ah, 09h
        int 21h
        mov ax, 4C00h
        int 21h
msg     db "TONE done", 13, 10, "$"
