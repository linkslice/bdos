; TIMER.COM - hook INT 8, run the timer at 1000 Hz for 18 BIOS ticks (~1 s)
; and count how many interrupts came: about 1000.
        org 100h
        cpu 386
        mov ax, 3508h           ; save the old handler
        int 21h
        mov [old8], bx
        mov [old8+2], es
        mov dx, tick
        mov ax, 2508h
        int 21h
        cli
        mov al, 36h             ; channel 0, mode 3
        out 43h, al
        mov ax, 1193            ; 1193182 / 1193 = 1000 Hz
        out 40h, al
        mov al, ah
        out 40h, al
        sti
        ; wait for 18 BIOS ticks, which our handler keeps at 18.2 Hz
        xor ax, ax
        mov es, ax
        mov eax, [es:046Ch]
        add eax, 18
        mov [until], eax
.w:     hlt
        mov eax, [es:046Ch]
        cmp eax, [until]
        jb .w
        cli
        mov al, 36h
        out 43h, al
        xor al, al              ; back to 18.2 Hz
        out 40h, al
        out 40h, al
        push ds
        lds dx, [old8]
        mov ax, 2508h
        int 21h
        pop ds
        sti
        mov dx, msg
        call print
        mov eax, [count]
        call dec32
        call crlf
        mov ax, 4C00h
        int 21h

; our handler: count, and call the BIOS's every 55 ms worth
tick:   inc dword [cs:count]
        inc word [cs:subcnt]
        cmp word [cs:subcnt], 55
        jb .eoi
        mov word [cs:subcnt], 0
        jmp far [cs:old8]       ; the BIOS updates 046C and sends the EOI
.eoi:   push ax
        mov al, 20h
        out 20h, al
        pop ax
        iret

%include "dos/lib.inc"

msg     db 'TIMER: interrupts in 18 ticks: $'
old8    dd 0
count   dd 0
subcnt  dw 0
until   dd 0
