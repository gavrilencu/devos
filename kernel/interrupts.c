#include "interrupts.h"
#include "kprintf.h"
#include "pic.h"
#include "vga.h"
#include "task.h"
#include "syscall.h"

static const char *exc_name[32] = {
    "#DE impartire la zero", "#DB debug",              "NMI",
    "#BP breakpoint",        "#OF overflow",           "#BR depasire limite",
    "#UD instructiune invalida", "#NM FPU indisponibil",
    "#DF double fault",      "(rezervat)",             "#TS TSS invalid",
    "#NP segment absent",    "#SS stack fault",        "#GP protectie generala",
    "#PF page fault",        "(rezervat)",             "#MF eroare FPU",
    "#AC aliniere",          "#MC machine check",      "#XM SIMD",
    "#VE virtualizare",      "#CP control protection", "(rezervat)",
    "(rezervat)", "(rezervat)", "(rezervat)", "(rezervat)", "(rezervat)",
    "(rezervat)", "(rezervat)", "(rezervat)", "(rezervat)",
};

static irq_handler_t irq_handlers[16];

void irq_install(int irq, irq_handler_t h)
{
    irq_handlers[irq] = h;
}

/* O exceptie netratata opreste sistemul, dar macar spune de ce:
 * ce exceptie a fost, unde s-a produs si starea completa a registrelor. */
static void panic_exception(struct int_frame *f)
{
    /* uint64_t e "unsigned long" pe Linux si "unsigned long long" pe alte
     * tinte; castul explicit face %ll corect peste tot. */
#define R(x) ((unsigned long long)(x))

    /* mesajul de panica trebuie sa fie VIZIBIL: comutam pe terminalul
     * task-ului care a cauzat-o */
    console_switch(task_current_term());
    con_color(VGA_WHITE, VGA_RED);
    kprintf("\nPANICA: exceptia %llu - %s (err=0x%llx)\n",
            R(f->vector), exc_name[f->vector], R(f->error));

    if (f->vector == 14) {
        uint64_t cr2;
        __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
        kprintf("CR2 (adresa care a cauzat page fault): %p\n", (void *)cr2);
    }

    kprintf("RIP=%016llx CS=%02llx RFLAGS=%016llx\n", R(f->rip), R(f->cs), R(f->rflags));
    kprintf("RSP=%016llx SS=%02llx\n", R(f->rsp), R(f->ss));
    kprintf("RAX=%016llx RBX=%016llx RCX=%016llx\n", R(f->rax), R(f->rbx), R(f->rcx));
    kprintf("RDX=%016llx RSI=%016llx RDI=%016llx\n", R(f->rdx), R(f->rsi), R(f->rdi));
    kprintf("RBP=%016llx R8 =%016llx R9 =%016llx\n", R(f->rbp), R(f->r8),  R(f->r9));
    kprintf("R10=%016llx R11=%016llx R12=%016llx\n", R(f->r10), R(f->r11), R(f->r12));
    kprintf("R13=%016llx R14=%016llx R15=%016llx\n", R(f->r13), R(f->r14), R(f->r15));
    kprintf("Sistemul e oprit.\n");
#undef R

    for (;;)
        __asm__ volatile("cli; hlt");
}

/* Intoarce cadrul cu care se face iretq: acelasi pentru majoritatea
 * intreruperilor, sau cadrul altui task cand schedulerul comuta contextul
 * (la tick-ul de timer sau la yield, vectorul 48). */
uint64_t isr_dispatch(struct int_frame *f);

uint64_t isr_dispatch(struct int_frame *f)
{
    /* #BP (int3) e singura exceptie pe care o tratam "bland" — utila ca
     * self-test si, mai tarziu, pentru debugging. */
    if (f->vector == 3) {
        kprintf("[ok] IDT functioneaza: #BP (breakpoint) tratat la RIP=%p\n",
                (void *)f->rip);
        return (uint64_t)f;
    }

    if (f->vector < 32) {
        /* O exceptie venita din ring 3 nu doboara sistemul: omoram doar
         * task-ul vinovat si mergem mai departe. Asta e izolarea. */
        if ((f->cs & 3) == 3) {
            con_color(VGA_LIGHT_RED, VGA_BLACK);
            kprintf("\n[kernel] Task user omorat: exceptia %llu - %s "
                    "(err=0x%llx) la RIP=%p\n",
                    (unsigned long long)f->vector, exc_name[f->vector],
                    (unsigned long long)f->error, (void *)f->rip);
            if (f->vector == 14) {
                uint64_t cr2;
                __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
                kprintf("[kernel] adresa accesata ilegal: %p\n", (void *)cr2);
            }
            con_color(VGA_LIGHT_GREY, VGA_BLACK);
            task_kill_current();
            return sched_tick((uint64_t)f);
        }
        panic_exception(f);            /* din kernel: nu se intoarce */
    }

    if (f->vector < 48) {
        int irq = (int)(f->vector - 32);
        if (irq_handlers[irq])
            irq_handlers[irq](f);
        pic_send_eoi(irq);             /* EOI inainte de eventuala comutare! */
        if (irq == 0)
            return sched_tick((uint64_t)f);
        return (uint64_t)f;
    }

    if (f->vector == 48)               /* yield voluntar */
        return sched_tick((uint64_t)f);

    if (f->vector == 128)              /* syscall din ring 3 */
        return syscall_handler(f);

    return (uint64_t)f;
}
