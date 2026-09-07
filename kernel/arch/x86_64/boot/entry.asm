; =============================================================================
; MyOS — punctul de intrare al kernelului (64-bit)
; stage2 sare direct la 0x9000; linker script-ul garanteaza ca sectiunea
; .entry (deci _start) e prima in binar.
; =============================================================================

[bits 64]

section .entry
global _start
extern kmain
extern __bss_start
extern __bss_end

_start:
    ; Curatam .bss — obiectul binar de pe disc nu contine sectiunea, deci
    ; memoria de acolo are gunoi la boot.
    cld
    lea rdi, [rel __bss_start]
    lea rcx, [rel __bss_end]
    sub rcx, rdi
    xor eax, eax
    rep stosb

    lea rsp, [rel stack_top]    ; stiva kernelului, 16 KiB in .bss
    call kmain

.hang:
    hlt
    jmp .hang

section .bss
align 16
global stack_top            ; folosit de TSS/scheduler ca stiva task-ului 0
stack_bottom: resb 16384
stack_top:
