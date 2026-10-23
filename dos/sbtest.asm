; SBTEST.COM - the Sound Blaster: find it, play half a second of a 441 Hz
; tone by DMA and count the interrupt at the end, then the same note on
; the OPL2's FM synthesizer. The test suite records what comes out.
        cpu 486
        org 100h

start:  mov dx, title
        call print
        ; shrink to 64 KB and borrow 128 KB: a buffer that doesn't cross a 64 KB page
        mov ah, 4Ah
        mov bx, 1000h
        int 21h
        mov ah, 48h
        mov bx, 2000h
        int 21h
        jc fail
        add ax, 0FFFh
        and ax, 0F000h
        mov [bufseg], ax

        ; reset the DSP: 1, 0, then AAh
        mov dx, 226h
        mov al, 1
        out dx, al
        mov cx, 100
.d1:    in al, dx
        loop .d1
        xor al, al
        out dx, al
        mov cx, 1000
.wait:  mov dx, 22Eh
        in al, dx
        test al, 80h
        jnz .got
        loop .wait
        jmp fail
.got:   mov dx, 22Ah
        in al, dx
        cmp al, 0AAh
        jne fail
        mov al, 0E1h            ; version
        call dsp_out
        call dsp_in
        mov [ver], al
        call dsp_in
        mov [ver+1], al
        mov dx, msg_dsp
        call print
        movzx eax, byte [ver]
        call dec32
        mov dl, '.'
        mov ah, 2
        int 21h
        movzx eax, byte [ver+1]
        call dec32
        call crlf

        ; the tone: 441 Hz square at 22050 Hz, 8-bit unsigned, 0.5 s
        push es
        mov es, [bufseg]
        xor di, di
        mov cx, 11025
        xor bx, bx
.fill:  mov al, 0C0h
        cmp bx, 25
        jb .hi
        mov al, 40h
.hi:    stosb
        inc bx
        cmp bx, 50
        jb .n
        xor bx, bx
.n:     loop .fill
        pop es

        ; IRQ 7 is INT 0Fh
        mov ax, 350Fh
        int 21h
        mov [oldirq], bx
        mov [oldirq+2], es
        mov ax, 250Fh
        mov dx, irq
        int 21h
        in al, 21h
        and al, 7Fh
        out 21h, al

        ; DMA channel 1: mask, mode (single, memory to card), address, page, count, unmask
        mov al, 5
        out 0Ah, al
        xor al, al
        out 0Ch, al
        mov al, 49h
        out 0Bh, al
        mov ax, [bufseg]
        mov bx, ax
        shl ax, 4               ; offset within the 64 KB page: 0 (aligned)
        out 02h, al
        mov al, ah
        out 02h, al
        mov al, bh
        shr al, 4               ; the page
        out 83h, al
        mov ax, 11025 - 1
        out 03h, al
        mov al, ah
        out 03h, al
        mov al, 1
        out 0Ah, al

        mov al, 0D1h            ; speaker on
        call dsp_out
        mov al, 41h             ; 22050 Hz
        call dsp_out
        mov al, 56h
        call dsp_out
        mov al, 22h
        call dsp_out
        mov al, 0C0h            ; 8-bit, single cycle
        call dsp_out
        mov al, 00h             ; mono, unsigned
        call dsp_out
        mov ax, 11025 - 1
        call dsp_out
        mov al, ah
        call dsp_out

        ; wait for the end-of-block interrupt (up to 3 s of timer ticks)
        mov ah, 0
        int 1Ah
        mov si, dx
.wirq:  sti
        cmp word [irqs], 0
        jne .irqok
        mov ah, 0
        int 1Ah
        sub dx, si
        cmp dx, 55
        jb .wirq
        mov dx, msg_noirq
        call print
        jmp fail
.irqok: mov dx, msg_irq
        call print

        ; the OPL2: find it with its timers, the way AdLib software does
        mov ah, 4
        mov al, 60h
        call opl
        mov al, 80h
        call opl
        mov dx, 388h
        in al, dx
        mov bl, al
        mov ah, 2
        mov al, 0FFh
        call opl
        mov ah, 4
        mov al, 21h
        call opl
        mov cx, 200
.del:   in al, dx
        loop .del
        mov bh, al
        mov ah, 4
        mov al, 60h
        call opl
        mov al, 80h
        call opl
        and bl, 0E0h
        and bh, 0E0h
        cmp bl, 0
        jne .noopl
        cmp bh, 0C0h
        jne .noopl
        mov dx, msg_opl
        call print

        ; A4 on channel 0: the carrier alone, a sine, sustained
        mov ah, 01h             ; waveform select on
        mov al, 20h
        call opl
        mov ah, 20h
        mov al, 01h
        call opl
        mov ah, 40h
        mov al, 3Fh             ; modulator silent
        call opl
        mov ah, 60h
        mov al, 0F0h
        call opl
        mov ah, 80h
        mov al, 0Fh
        call opl
        mov ah, 23h
        mov al, 21h             ; sustained, multiple 1
        call opl
        mov ah, 43h
        mov al, 00h             ; loudest
        call opl
        mov ah, 63h
        mov al, 0F0h
        call opl
        mov ah, 83h
        mov al, 0Fh
        call opl
        mov ah, 0C0h
        mov al, 01h             ; additive
        call opl
        mov ah, 0A0h
        mov al, 44h             ; F-number 580 (44h + 2 << 8), block 4: 440 Hz
        call opl
        mov ah, 0B0h
        mov al, 32h             ; key on
        call opl
        mov ah, 0
        int 1Ah
        mov si, dx
.note:  mov ah, 0
        int 1Ah
        sub dx, si
        cmp dx, 10              ; about half a second
        jb .note
        mov ah, 0B0h
        mov al, 12h             ; key off
        call opl
        jmp done

.noopl: mov dx, msg_noopl
        call print
done:   push ds
        lds dx, [oldirq]
        mov ax, 250Fh
        int 21h
        pop ds
        mov dx, msg_done
        call print
        mov ax, 4C00h
        int 21h

fail:   mov dx, msg_fail
        call print
        mov ax, 4C01h
        int 21h

irq:    push ax
        push dx
        push ds
        push cs
        pop ds
        inc word [irqs]
        mov dx, 22Eh            ; acknowledge the DSP
        in al, dx
        mov al, 20h
        out 20h, al
        pop ds
        pop dx
        pop ax
        iret

; AL to the DSP
dsp_out:
        push dx
        push ax
        mov dx, 22Ch
.w:     in al, dx
        test al, 80h
        jnz .w
        pop ax
        out dx, al
        pop dx
        ret

; a byte from the DSP into AL
dsp_in: push dx
        mov dx, 22Eh
.w:     in al, dx
        test al, 80h
        jz .w
        mov dx, 22Ah
        in al, dx
        pop dx
        ret

; OPL register AH = AL
opl:    push dx
        push ax
        mov dx, 388h
        mov al, ah
        out dx, al
        in al, dx
        in al, dx
        pop ax
        inc dx
        out dx, al
        dec dx
        in al, dx
        in al, dx
        in al, dx
        pop dx
        ret

%include "dos/lib.inc"

title   db "Sound Blaster test", 13, 10, "$"
msg_dsp db "DSP version $"
msg_irq db "DMA done, IRQ 7 came", 13, 10, "$"
msg_noirq db "no IRQ", 13, 10, "$"
msg_opl db "OPL2 found", 13, 10, "$"
msg_noopl db "no OPL2", 13, 10, "$"
msg_done db "Sound test done", 13, 10, "$"
msg_fail db "Sound test FAILED", 13, 10, "$"
bufseg  dw 0
ver     db 0, 0
irqs    dw 0
oldirq  dd 0
