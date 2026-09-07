; =============================================================================
; MyOS — GDT-ul kernelului
; Pana acum foloseam GDT-ul incarcat de stage2 (care ramane in memoria de la
; 0x8000). Kernelul isi instaleaza aici propriul GDT, ca sa nu depinda de
; bootloader. Selectori noi: 0x08 = cod 64-bit, 0x10 = date.
; =============================================================================

[bits 64]

section .data
align 16
gdt64:
    dq 0                        ; 0x00: descriptor nul
    dq 0x00AF9A000000FFFF       ; 0x08: cod kernel 64-bit (L=1, DPL=0)
    dq 0x00CF92000000FFFF       ; 0x10: date kernel
    dq 0x00AFFA000000FFFF       ; 0x18: cod user 64-bit (DPL=3) -> selector 0x1B
    dq 0x00CFF2000000FFFF       ; 0x20: date user (DPL=3)       -> selector 0x23
global gdt64_tss_desc
gdt64_tss_desc:                 ; 0x28: descriptor TSS (16 bytes in long mode),
    dq 0                        ;       completat la runtime de tss_init()
    dq 0
gdt64_ptr:
    dw $ - gdt64 - 1
    dq gdt64

section .text
global gdt_init
gdt_init:
    lgdt [rel gdt64_ptr]

    ; Reincarcam CS printr-un far return (nu se poate cu mov)
    push qword 0x08
    lea rax, [rel .reload]
    push rax
    retfq
.reload:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax
    ret
