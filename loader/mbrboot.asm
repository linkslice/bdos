; mbrboot.asm - boot code for the protective MBR of a disk bdos installed
; on: a BIOS that tries to boot it hears why it can't, rather than nothing.
; It has to fit in the 440 bytes before the disk signature.
        bits 16
        org 7C00h
        xor ax, ax
        mov ds, ax
        mov ss, ax
        mov sp, 7C00h
        mov si, msg
.next:  lodsb
        or al, al
        jz .done
        mov ah, 0Eh
        mov bx, 0007h
        int 10h
        jmp .next
.done:  xor ax, ax
        int 16h
        int 18h                 ; the next boot device
msg:    db 13, 10, "This disk has bdos on it, which needs UEFI firmware.", 13, 10
        db "Proxmox: Hardware > BIOS > OVMF (UEFI), plus an EFI Disk with", 13, 10
        db "Pre-Enroll keys unticked. A PC: UEFI boot, Secure Boot off.", 13, 10
        db "Press a key.", 0
%if ($ - $$) > 440
%error "mbrboot is too big"
%endif
