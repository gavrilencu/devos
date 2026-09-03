; =============================================================================
; MyOS — stub-urile de intreruperi
; CPU-ul nu salveaza registrele generale la o intrerupere, doar
; SS:RSP, RFLAGS, CS:RIP (si uneori un cod de eroare). Stub-urile de aici
; uniformizeaza stiva (cod de eroare fals 0 unde CPU-ul nu pune unul),
; salveaza toate registrele si cheama isr_dispatch() din C cu un pointer
; la cadrul salvat (struct int_frame).
; =============================================================================

[bits 64]

extern isr_dispatch

section .text

; Exceptiile 8, 10-14, 17 si 21 primesc cod de eroare de la CPU;
; pentru restul punem noi un 0, ca stiva sa arate identic in ambele cazuri.
%macro DEF_ISR 1
global isr%1
isr%1:
%if (%1 == 8) || (%1 == 10) || (%1 == 11) || (%1 == 12) || (%1 == 13) || (%1 == 14) || (%1 == 17) || (%1 == 21)
    push qword %1               ; codul de eroare e deja pe stiva
%else
    push qword 0                ; cod de eroare fals
    push qword %1
%endif
    jmp isr_common
%endmacro

; Vectorii 0-47: exceptii + IRQ-uri; vectorul 48: yield (comutare voluntara).
%assign i 0
%rep 49
    DEF_ISR i
%assign i i+1
%endrep

; Vectorul 128 (0x80): syscall — poarta lui din IDT are DPL=3,
; deci poate fi apelat cu "int 0x80" din ring 3.
DEF_ISR 128

isr_common:
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    cld                         ; ABI-ul C cere DF=0
    mov rdi, rsp                ; primul argument: pointer la struct int_frame
    call isr_dispatch

    ; isr_dispatch intoarce cadrul task-ului care trebuie sa ruleze:
    ; acelasi (fara comutare) sau al altui task (comutare de context).
    mov rsp, rax

    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    add rsp, 16                 ; scoatem vectorul si codul de eroare
    iretq

; Tabela cu adresele stub-urilor, folosita de idt.c
section .rodata
global isr_stub_table
isr_stub_table:
%assign i 0
%rep 49
    dq isr%+i
%assign i i+1
%endrep
