; =============================================================================
; MyOS — stage1 (boot sector)
; BIOS-ul ne incarca la 0x7C00 si ne da drive-ul de boot in DL.
; Rol: citim de pe disc stage2 + kernelul (120 sectoare incepand cu LBA 1)
; la adresa 0x8000, apoi sarim in stage2.
;
; Layout disc:                       Layout memorie dupa stage1:
;   LBA 0      : stage1 (acest cod)   0x7C00        : stage1
;   LBA 1..8   : stage2 (4 KiB)       0x8000-0x8FFF : stage2
;   LBA 9..192 : kernel               0x9000-...    : kernel
; =============================================================================

[org 0x7C00]
[bits 16]

CHUNKS        equ 48            ; 48 bucati x 8 sectoare = 384 sectoare (192 KiB)
SECT_PER_READ equ 8

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00              ; stiva creste in jos, sub bootloader
    sti
    cld
    mov [boot_drive], dl

    mov si, msg_boot
    call print16

    ; Verificam ca BIOS-ul suporta extensiile int 13h (citire LBA)
    mov ah, 0x41
    mov bx, 0x55AA
    mov dl, [boot_drive]
    int 0x13
    jc disk_error
    cmp bx, 0xAA55
    jne disk_error

    ; Citim in bucati de 8 sectoare ca sa nu trecem de limita de 64 KiB
    ; a unui singur transfer BIOS. Bufferul avanseaza prin segment (0x100
    ; paragrafe = 4096 bytes = 8 sectoare).
    mov cx, CHUNKS
.read_loop:
    push cx
    mov si, dap
    mov ah, 0x42
    mov dl, [boot_drive]
    int 0x13
    jc disk_error
    add word [dap.lba], SECT_PER_READ
    add word [dap.seg], 0x0100
    pop cx
    loop .read_loop

    mov si, msg_ok
    call print16

    jmp 0x0000:0x8000           ; predam controlul lui stage2

disk_error:
    mov si, msg_err
    call print16
.halt:
    hlt
    jmp .halt

; print16: afiseaza sirul terminat cu 0 de la DS:SI (BIOS teletype)
print16:
    mov ah, 0x0E
.loop:
    lodsb
    test al, al
    jz .done
    int 0x10
    jmp .loop
.done:
    ret

boot_drive: db 0

; Disk Address Packet pentru int 13h AH=42h
align 4
dap:
    db 0x10, 0
.count: dw SECT_PER_READ
.off:   dw 0x0000
.seg:   dw 0x0800               ; 0x0800:0x0000 = 0x8000 liniar
.lba:   dq 1

msg_boot: db "MyOS stage1: citesc discul...", 13, 10, 0
msg_ok:   db "MyOS stage1: OK", 13, 10, 0
msg_err:  db "Eroare de disc!", 13, 10, 0

times 510-($-$$) db 0
dw 0xAA55                       ; semnatura de boot
