; =============================================================================
; Trampolina de pornire a nucleelor secundare (AP) — Milestone 57 SMP.
; Un AP porneste in REAL MODE (16-bit) la adresa (vector<<12) = 0x8000, dupa
; secventa INIT-SIPI-SIPI trimisa de BSP prin Local APIC. Aici il ducem prin
; protected mode -> long mode, incarcand ACELEASI tabele de paginare ca ale
; kernelului (CR3 pasat de BSP), apoi sarim intr-o functie C (ap_entry).
;
; BSP-ul copiaza acest cod la 0x8000 si completeaza parametrii:
;   0x8FF0 : CR3 (PML4-ul kernelului)          (qword)
;   0x8FE0 : varful stivei acestui AP          (qword)
;   0x8FD0 : adresa functiei ap_entry (64-bit) (qword)
; =============================================================================

[bits 16]
[org 0x8000]

CR3_PTR    equ 0x8FF0
STACK_PTR  equ 0x8FE0
ENTRY_PTR  equ 0x8FD0

CODE32_SEL equ 0x08
DATA_SEL   equ 0x10
CODE64_SEL equ 0x18

start:
    cli
    cld
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    lgdt [gdt_desc]

    mov eax, cr0
    or  eax, 1                  ; CR0.PE
    mov cr0, eax
    jmp CODE32_SEL:pm32

[bits 32]
pm32:
    mov ax, DATA_SEL
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax

    mov eax, [CR3_PTR]          ; PML4-ul kernelului (pasat de BSP)
    mov cr3, eax

    mov eax, cr4
    or  eax, 1 << 5             ; CR4.PAE
    mov cr4, eax

    mov ecx, 0xC0000080         ; EFER
    rdmsr
    or  eax, (1 << 8) | (1 << 11)   ; LME (long mode) + NXE (ca la BSP)
    wrmsr

    mov eax, cr0
    or  eax, 1 << 31            ; CR0.PG
    mov cr0, eax
    jmp CODE64_SEL:lm64

[bits 64]
lm64:
    mov ax, DATA_SEL
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax

    mov rsp, [STACK_PTR]        ; stiva proprie a acestui AP
    mov rax, [ENTRY_PTR]
    call rax                    ; ap_entry() — nu se intoarce
.hang:
    cli
    hlt
    jmp .hang

align 8
gdt:
    dq 0
    dq 0x00CF9A000000FFFF       ; 0x08: cod 32-bit
    dq 0x00CF92000000FFFF       ; 0x10: date
    dq 0x00AF9A000000FFFF       ; 0x18: cod 64-bit (bitul L)
gdt_desc:
    dw gdt_desc - gdt - 1
    dd gdt
