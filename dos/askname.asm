; ASKNAME.COM - INT 21h buffered keyboard input
        org 100h
        mov dx, prompt
        mov ah, 09h
        int 21h
        mov dx, inbuf
        mov ah, 0Ah
        int 21h
        mov dx, hello
        mov ah, 09h
        int 21h
        mov cl, [inbuf + 1]
        xor ch, ch
        mov si, inbuf + 2
.out:   jcxz .end
        lodsb
        mov dl, al
        mov ah, 02h
        int 21h
        loop .out
.end:   mov dx, bang
        mov ah, 09h
        int 21h
        mov ax, 4C00h
        int 21h
prompt  db 'What is your name? $'
hello   db 13, 10, 'Pleased to meet you, $'
bang    db '!', 13, 10, '$'
inbuf   db 40, 0
        times 41 db 0
