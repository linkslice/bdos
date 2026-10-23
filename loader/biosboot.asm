; biosboot.asm - the ISO's boot image for PCs that start it the old (BIOS)
; way: bdos is UEFI only, so say what to change instead of "no bootable
; device". El Torito no-emulation: loaded at 0000:7C00.
        bits 16
        org 7C00h
        cli
        xor ax, ax
        mov ds, ax
        mov ss, ax
        mov sp, 7C00h
        sti
        mov ax, 0003h           ; a clean 80x25 screen
        int 10h
        mov si, msg
.next:  lodsb
        or al, al
        jz .done
        mov ah, 0Eh
        mov bx, 0007h
        int 10h
        jmp .next
.done:  xor ax, ax              ; a key restarts, to try another device
        int 16h
        int 19h
        jmp .done

msg:    db 13, 10
        db "  bdos needs a UEFI machine, and this one started the CD the old (BIOS) way.", 13, 10
        db 13, 10
        db "  Proxmox VE:  Hardware > BIOS > OVMF (UEFI), then Add > EFI Disk with", 13, 10
        db "               'Pre-Enroll keys' unticked (bdos isn't signed for Secure", 13, 10
        db "               Boot). Machine q35 or i440fx both work.", 13, 10
        db "  QEMU:        add OVMF, e.g. -bios /usr/share/OVMF/OVMF_CODE.fd", 13, 10
        db "  A real PC:   choose the CD's UEFI entry in the boot menu, with Secure", 13, 10
        db "               Boot turned off in the firmware settings.", 13, 10
        db 13, 10
        db "  If the firmware is UEFI and says Access Denied, Secure Boot is on.", 13, 10
        db 13, 10
        db "  Press a key to try the next boot device.", 13, 10, 0
        times 2048 - ($ - $$) db 0
