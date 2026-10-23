; DPMITEST.COM - exercise the DPMI host: the switch to protected mode,
; descriptors, memory, 32-bit code, INT 31h calls down to real mode,
; real-mode callbacks, an exception handler and a timer interrupt handler.
; Each step prints "ok". The test suite runs it.
        cpu 486
        org 100h
        bits 16

start:  mov dx, title
        call print
        ; give back what a .COM doesn't need, so there's room for the host's data
        mov ah, 4Ah
        mov bx, 1000h           ; 64 KB
        int 21h
        mov ax, 1687h
        int 2Fh
        or ax, ax
        jz .have
        mov dx, nohost
        call print
        mov ax, 4C01h
        int 21h
.have:  mov [entry], di
        mov [entry+2], es
        mov [hostparas], si
        mov [ver], dx
        mov dx, msg_ver
        call print
        movzx eax, byte [ver+1]
        call dec32
        mov dl, '.'
        mov ah, 02h
        int 21h
        movzx eax, byte [ver]
        call dec32
        call crlf
        mov [rmseg], cs
        ; the host's data area
        mov ah, 48h
        mov bx, [hostparas]
        int 21h
        jc fail
        mov es, ax
        mov ax, 1               ; a 32-bit client
        call far [entry]
        jc fail
        bits 16
        ; protected mode now, in a 16-bit code segment
        mov [csel], cs
        mov [dsel], ds
        mov dx, msg_pm
        call pmprint16
        mov ax, cs
        and ax, 3
        cmp ax, 3
        jne pmfail16

        ; 1. a 32-bit code segment over this program, and a flat data segment
        mov ax, 0000h
        mov cx, 2
        int 31h
        jc pmfail16
        mov [code32], ax
        add ax, 8               ; the selector increment, which 0003h confirms below
        mov [flat], ax
        mov ax, 0003h
        int 31h
        cmp ax, 8
        jne pmfail16
        mov ax, 0006h           ; the base of our code segment
        mov bx, cs
        int 31h
        jc pmfail16
        mov [base], dx
        mov [base+2], cx
        mov bx, [code32]
        mov ax, 0007h
        int 31h                 ; same base (CX:DX)
        jc pmfail16
        mov ax, 0008h
        xor cx, cx
        mov dx, 0FFFFh
        int 31h
        jc pmfail16
        mov ax, 0009h
        mov cl, 0FAh            ; present, ring 3, code, readable
        mov ch, 40h             ; 32-bit
        int 31h
        jc pmfail16
        mov bx, [flat]
        mov ax, 0008h
        mov cx, 0FFFFh
        mov dx, 0FFFFh          ; 4 GB
        int 31h
        jc pmfail16
        mov ax, 0009h
        mov cl, 0F2h
        mov ch, 0C0h            ; 4K granular, 32-bit
        int 31h
        jc pmfail16
        mov dx, msg_ok1
        call pmprint16
        mov ax, [code32]
        mov [jmp32+2], ax
        jmp far [jmp32]

pmfail16:
        mov dx, msg_fail
        call pmprint16
        mov ax, 4C01h
        int 21h

; print the $-terminated string at DS:DX, a character at a time (INT 21h 02h, reflected)
pmprint16:
        push si
        push dx
        mov si, dx
.l:     lodsb
        cmp al, '$'
        je .e
        mov dl, al
        mov ah, 02h
        int 21h
        jmp .l
.e:     pop dx
        pop si
        ret

        bits 32
pm32:   mov dx, msg_32
        call pmprint32

        ; 2. a megabyte of memory, through the flat selector
        mov ax, 0501h
        mov bx, 10h             ; 1 MB
        xor cx, cx
        int 31h
        jc pmfail32
        mov [memh], di
        mov [memh+2], si
        shl ebx, 16
        mov bx, cx
        mov [memlin], ebx
        push es
        mov es, [flat]
        mov edi, ebx
        mov ecx, 100000h / 4
        mov eax, 12345678h
        cld
        rep stosd
        mov edi, [memlin]
        add edi, 0FFFFCh
        cmp dword [es:edi], 12345678h
        pop es
        jne pmfail32
        mov ax, 0500h           ; free memory info
        push es
        push ds
        pop es
        mov edi, meminfo
        int 31h
        pop es
        jc pmfail32
        cmp dword [meminfo], 100000h
        jb pmfail32
        mov ax, 0502h
        mov si, [memh+2]
        mov di, [memh]
        int 31h
        jc pmfail32
        mov dx, msg_ok2
        call pmprint32

        ; 3. INT 21h AH=09h for real, through 0300h, with DS = our real-mode segment
        mov dword [rm_eax], 0900h
        mov ax, [rmseg]
        mov [rm_ds], ax
        mov dword [rm_edx], rmstring
        mov word [rm_ss], 0
        mov word [rm_sp], 0
        mov word [rm_flags], 0
        push es
        push ds
        pop es
        mov edi, rmregs
        mov ax, 0300h
        mov bx, 21h
        xor cx, cx
        int 31h
        pop es
        jc pmfail32

        ; 4. a real-mode callback, called from real mode through 0301h
        push ds
        push es
        mov ax, ds
        mov es, ax
        mov edi, cbregs
        mov ds, [cs:code32sel]
        mov esi, cbproc
        mov ax, 0303h
        int 31h
        pop es
        pop ds
        jc pmfail32
        mov [rm_ip], dx
        mov [rm_cs], cx
        mov word [rm_ss], 0
        mov word [rm_sp], 0
        mov dword [rm_eax], 0
        push es
        push ds
        pop es
        mov edi, rmregs
        mov ax, 0301h
        xor bx, bx
        xor cx, cx
        int 31h
        pop es
        jc pmfail32
        cmp dword [rm_eax], 0C0FFEEh
        jne pmfail32
        mov dx, msg_ok4
        call pmprint32

        ; 5. an exception handler: HLT at ring 3 is a #GP, which we step over
        mov ax, 0202h
        mov bl, 0Dh
        int 31h
        mov [oldexc], edx
        mov [oldexc+4], cx
        mov ax, 0203h
        mov bl, 0Dh
        mov cx, [code32]
        mov edx, gp_handler
        int 31h
        jc pmfail32
        hlt
        cmp byte [gp_seen], 1
        jne pmfail32
        mov ax, 0203h
        mov bl, 0Dh
        mov cx, [oldexc+4]
        mov edx, [oldexc]
        int 31h
        mov dx, msg_ok5
        call pmprint32

        ; 6. a timer handler (IRQ 0), chained to the old one
        mov ax, 0204h
        mov bl, 08h
        int 31h
        mov [oldint8], edx
        mov [oldint8+4], cx
        mov ax, 0205h
        mov bl, 08h
        mov cx, [code32]
        mov edx, timer
        int 31h
        jc pmfail32
        sti
        mov ecx, 50000000
.wait:  cmp dword [ticks], 5
        jae .got
        dec ecx
        jnz .wait
        jmp pmfail32
.got:   mov ax, 0205h
        mov bl, 08h
        mov cx, [oldint8+4]
        mov edx, [oldint8]
        int 31h
        mov dx, msg_ok6
        call pmprint32

        ; 7. INT 21h 25h/35h in protected mode mean protected-mode vectors
        mov ax, 3531h
        int 21h
        mov ax, es
        or ax, ax
        jz pmfail32
        mov dx, msg_ok7
        call pmprint32

        mov dx, msg_done
        call pmprint32
        mov ax, 4C00h
        int 21h

pmfail32:
        mov dx, msg_fail
        call pmprint32
        mov ax, 4C01h
        int 21h

pmprint32:
        push esi
        push edx
        movzx esi, dx
.l:     lodsb
        cmp al, '$'
        je .e
        mov dl, al
        mov ah, 02h
        int 21h
        jmp .l
.e:     pop edx
        pop esi
        ret

; the #GP handler: [esp] ret eip, ret cs, err, eip, cs, eflags, esp, ss (dwords)
gp_handler:
        push ds
        mov ds, [cs:dsel]
        mov byte [gp_seen], 1
        pop ds
        inc dword [esp + 12]    ; past the HLT
        retf

timer:  push ds
        mov ds, [cs:dsel]
        inc dword [ticks]
        pop ds
        jmp far [cs:oldint8]

; the callback: DS:ESI = the real-mode stack (a far return address), ES:EDI = cbregs
cbproc: mov ax, [esi]
        mov [es:edi + 42], ax   ; IP
        mov ax, [esi + 2]
        mov [es:edi + 44], ax   ; CS
        add word [es:edi + 46], 4       ; SP: as if it RETFed
        mov dword [es:edi + 28], 0C0FFEEh       ; EAX
        iretd

        bits 16
fail:   mov dx, msg_fail
        call print
        mov ax, 4C01h
        int 21h

%include "dos/lib.inc"

title   db "DPMI test", 13, 10, "$"
nohost  db "no DPMI host", 13, 10, "$"
msg_ver db "host version $"
msg_pm  db "ok 0 protected mode", 13, 10, "$"
msg_ok1 db "ok 1 descriptors", 13, 10, "$"
msg_32  db "   32-bit code", 13, 10, "$"
msg_ok2 db "ok 2 memory", 13, 10, "$"
rmstring db "ok 3 real-mode call", 13, 10, "$"
msg_ok4 db "ok 4 callback", 13, 10, "$"
msg_ok5 db "ok 5 exception", 13, 10, "$"
msg_ok6 db "ok 6 timer", 13, 10, "$"
msg_ok7 db "ok 7 vectors", 13, 10, "$"
msg_done db "DPMI test passed", 13, 10, "$"
msg_fail db "DPMI test FAILED", 13, 10, "$"

        align 4
entry   dd 0
hostparas dw 0
ver     dw 0
rmseg   dw 0
csel    dw 0
dsel    dw 0
code32  dw 0
code32sel equ code32
flat    dw 0
base    dd 0
jmp32   dw pm32, 0
memh    dd 0
memlin  dd 0
ticks   dd 0
gp_seen db 0
        align 4
oldexc  dd 0
        dw 0
        align 4
oldint8 dd 0
        dw 0
        align 4
meminfo times 48 db 0
rmregs:
rm_edi  dd 0
rm_esi  dd 0
rm_ebp  dd 0
        dd 0
rm_ebx  dd 0
rm_edx  dd 0
rm_ecx  dd 0
rm_eax  dd 0
rm_flags dw 0
rm_es   dw 0
rm_ds   dw 0
rm_fs   dw 0
rm_gs   dw 0
rm_ip   dw 0
rm_cs   dw 0
rm_sp   dw 0
rm_ss   dw 0
cbregs  times 50 db 0
