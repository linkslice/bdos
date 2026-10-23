; COUNTER.COM - counts the keys you press until Q, for trying saved states:
; press a few keys, Ctrl+Alt+F11 to save, a few more, Ctrl+Alt+F12 to go
; back. RESUME COUNTER carries on from the save another day.
        org 100h
        cpu 386
        mov dx, hello
        call print
.k:     xor ah, ah
        int 16h
        cmp al, 'q'
        je .q
        inc dword [count]
        mov dx, now
        call print
        mov eax, [count]
        call dec32
        call crlf
        jmp .k
.q:     mov dx, final
        call print
        mov eax, [count]
        call dec32
        call crlf
        mov ax, 4C00h
        int 21h

%include "dos/lib.inc"

hello   db 'COUNTER: press keys, q to quit', 13, 10, '$'
now     db 'count $'
final   db 'COUNTER final $'
count   dd 0
