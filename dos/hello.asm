; HELLO.COM - INT 21h text output and the command tail
        org 100h
        mov dx, msg
        mov ah, 09h
        int 21h
        ; echo the command tail (PSP:80h = length, 81h = text)
        mov dx, tailmsg
        mov ah, 09h
        int 21h
        mov cl, [80h]
        xor ch, ch
        mov si, 81h
.tail:  jcxz .done
        mov dl, [si]
        mov ah, 02h
        int 21h
        inc si
        dec cx
        jmp .tail
.done:  mov dx, crlf
        mov ah, 09h
        int 21h
        mov ax, 4C00h
        int 21h
msg     db 'Hello from 16-bit DOS on bdos!', 13, 10, '$'
tailmsg db 'Command tail:[$'
crlf    db ']', 13, 10, '$'
