; ENVPATH.COM - print the program's own path from the end of its environment,
; the way DOS extenders find their .EXE to load the rest of themselves from,
; and the PATH variable from the start of it.
        org 100h
        mov es, [2Ch]           ; the environment segment, from the PSP
        xor di, di
        mov dx, pathmsg
        call print
.var:   cmp byte [es:di], 0     ; the end of the variables: an empty one
        je .end
        cmp dword [es:di], 'PATH'
        jne .skip
        cmp byte [es:di + 4], '='
        jne .skip
        call puts               ; PATH=...
        jmp .var
.skip:  inc di                  ; to the next variable
        cmp byte [es:di - 1], 0
        jne .skip
        jmp .var
.end:   inc di                  ; past the empty one
        call crlf
        mov dx, selfmsg
        call print
        cmp word [es:di], 1     ; a count of strings that follow: one
        jne .bad
        add di, 2
        call puts
        call crlf
        mov ax, 4C00h
        int 21h
.bad:   mov dx, nomsg
        call print
        mov ax, 4C01h
        int 21h

; print the string at ES:DI, leaving DI past its zero
puts:   mov dl, [es:di]
        inc di
        or dl, dl
        jz .done
        mov ah, 02h
        int 21h
        jmp puts
.done:  ret

%include "dos/lib.inc"

pathmsg db 'ENVPATH var: $'
selfmsg db 'ENVPATH self: $'
nomsg   db 'no program path after the environment', 13, 10, '$'
