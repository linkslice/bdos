; HELLOEXE.EXE - a hand-built MZ executable with a relocated data segment
        org 0
hdr:    db 'MZ'
        dw (file_end - hdr) % 512
        dw (file_end - hdr + 511) / 512
        dw 1                            ; one relocation
        dw (code - hdr) / 16            ; header size in paragraphs
        dw 10h, 0FFFFh                  ; min/max extra memory
        dw (stack_area - code) / 16     ; SS, relative
        dw 100h                         ; SP
        dw 0                            ; checksum
        dw 0                            ; IP
        dw 0                            ; CS, relative
        dw reloc - hdr
        dw 0
reloc:  dw fixup + 1 - code, 0          ; the immediate in "mov ax, seg data"
        align 16, db 0
code:
fixup:  mov ax, (data - code) / 16      ; the loader adds the load segment
        mov ds, ax
        mov dx, msg - data
        mov ah, 09h
        int 21h
        mov ax, 4C05h                   ; exit code 5
        int 21h
        align 16, db 0
data:
msg:    db 'Hello from an .EXE with relocations!', 13, 10, '$'
        align 16, db 0
stack_area:
        times 256 db 0
file_end:
