#pragma once
#include <stdint.h>

/* Cadrul salvat pe stiva de stub-urile din isr.asm — ordinea campurilor
 * trebuie sa fie exact inversa ordinii push-urilor de acolo. */
struct int_frame {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector, error;
    uint64_t rip, cs, rflags, rsp, ss;
};

typedef void (*irq_handler_t)(struct int_frame *f);

void gdt_init(void);                          /* gdt.asm — GDT-ul kernelului */
void idt_init(void);                          /* idt.c   — tabela de intreruperi */
void irq_install(int irq, irq_handler_t h);   /* interrupts.c */
