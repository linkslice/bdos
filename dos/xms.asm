; XMS.COM - the XMS driver: allocate a megabyte, copy a message up into it
; and back down, and check the A20 line.
        org 100h
        cpu 386
        mov ax, 4300h
        int 2Fh
        cmp al, 80h
        jne .none
        mov ax, 4310h
        int 2Fh
        mov [xms], bx
        mov [xms+2], es
        mov ah, 08h             ; free memory
        call far [xms]
        mov dx, freemsg
        call print
        movzx eax, dx
        call dec32
        mov dx, kb
        call print
        mov ah, 09h             ; allocate 1024 KB
        mov dx, 1024
        call far [xms]
        or ax, ax
        jz .fail
        mov [handle], dx
        ; up: conventional (handle 0, seg:off) -> the block
        mov word [mv.dst_h], dx
        mov dword [mv.src_off], 0
        mov word [mv.src_off], msg
        mov [mv.src_off+2], ds
        mov dword [mv.dst_off], 1000h
        mov word [mv.src_h], 0
        mov si, mv
        mov ah, 0Bh
        call far [xms]
        or ax, ax
        jz .fail
        ; down: the block -> buf
        mov ax, [handle]
        mov [mv.src_h], ax
        mov dword [mv.src_off], 1000h
        mov word [mv.dst_h], 0
        mov word [mv.dst_off], buf
        mov [mv.dst_off+2], ds
        mov si, mv
        mov ah, 0Bh
        call far [xms]
        or ax, ax
        jz .fail
        mov dx, buf
        call print
        mov ah, 0Ah
        mov dx, [handle]
        call far [xms]
        mov ax, 4C00h
        int 21h
.fail:  mov dx, failmsg
        call print
        mov ax, 4C01h
        int 21h
.none:  mov dx, nonemsg
        call print
        mov ax, 4C02h
        int 21h

%include "dos/lib.inc"

xms     dd 0
handle  dw 0
mv:
mv.len  dd msglen
mv.src_h dw 0
mv.src_off dd 0
mv.dst_h dw 0
mv.dst_off dd 0
msg     db 'XMS: round trip through extended memory OK', 13, 10, '$'
msglen  equ $ - msg
freemsg db 'XMS: free $'
kb      db ' KB', 13, 10, '$'
failmsg db 'XMS: failed', 13, 10, '$'
nonemsg db 'XMS: no driver', 13, 10, '$'
buf     times 64 db 0
