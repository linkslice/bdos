; PARENT.COM - EXEC: run HELLO.COM as a child, then a bdos command through
; COMMAND.COM /C, and report the child's exit code.
        org 100h
        cpu 386
        mov sp, stack_top
        mov bx, (prog_end - $$ + 256 + 15) / 16
        mov ah, 4Ah             ; give back the memory we don't need
        int 21h
        mov dx, child
        mov bx, pblock
        mov [pblock+4], cs
        mov ax, 4B00h
        int 21h
        jc .fail
        mov ah, 4Dh
        int 21h
        mov dx, codemsg
        call print
        movzx eax, al
        call dec32
        call crlf
        ; COMMAND.COM /C runs a bdos command
        mov dx, comspec
        mov bx, pblock
        mov word [pblock+2], ctail
        mov ax, 4B00h
        int 21h
        jc .fail
        mov dx, back
        call print
        mov ax, 4C00h
        int 21h
.fail:  mov dx, failmsg
        call print
        call hex32
        call crlf
        mov ax, 4C01h
        int 21h

%include "dos/lib.inc"

child   db 'HELLO.COM', 0
comspec db 'C:\COMMAND.COM', 0
tail    db 9, ' from mum', 13
ctail   db 7, '/C VER', 13
pblock  dw 0, tail, 0, 5Ch, 0, 6Ch, 0
codemsg db 'PARENT: child exit code $'
back    db 'PARENT: back from COMMAND.COM', 13, 10, '$'
failmsg db 'PARENT: EXEC failed $'
        times 256 db 0
stack_top:
prog_end:
