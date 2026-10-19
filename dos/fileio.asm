; FILEIO.COM - create, write, read back, find files, exit code
        org 100h
        mov ah, 3Ch             ; create DOSTEST.TXT
        xor cx, cx
        mov dx, fname
        int 21h
        jc fail
        mov bx, ax
        mov ah, 40h             ; write
        mov cx, datalen
        mov dx, data
        int 21h
        jc fail
        mov ah, 3Eh             ; close
        int 21h
        mov ax, 3D00h           ; open for reading
        mov dx, fname
        int 21h
        jc fail
        mov bx, ax
        mov ah, 3Fh             ; read
        mov cx, 200
        mov dx, buf
        int 21h
        jc fail
        mov cx, ax              ; echo what came back to stdout
        mov ah, 40h
        mov bx, 1
        mov dx, buf
        int 21h
        mov ah, 4Eh             ; FindFirst *.TXT
        mov cx, 0
        mov dx, pattern
        int 21h
        jc .nofind
.found: mov si, 80h + 30        ; name in the DTA
.name:  lodsb
        or al, al
        jz .next
        mov dl, al
        mov ah, 02h
        int 21h
        jmp .name
.next:  mov dl, ' '
        mov ah, 02h
        int 21h
        mov ah, 4Fh
        int 21h
        jnc .found
.nofind:
        mov dx, crlf
        mov ah, 09h
        int 21h
        mov ax, 4C07h           ; exit code 7
        int 21h
fail:   mov dx, failmsg
        mov ah, 09h
        int 21h
        mov ax, 4C01h
        int 21h
fname   db 'DOSTEST.TXT', 0
pattern db '*.TXT', 0
data    db 'File written and read back by a DOS program.', 13, 10
datalen equ $ - data
failmsg db 'File I/O failed', 13, 10, '$'
crlf    db 13, 10, '$'
buf     times 200 db 0
