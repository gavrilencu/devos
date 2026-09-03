#include <stdint.h>
#include <stddef.h>
#include "task.h"
#include "interrupts.h"
#include "pmm.h"
#include "vmm.h"
#include "elf.h"
#include "pit.h"
#include "tss.h"
#include "vga.h"
#include "pipe.h"
#include "kprintf.h"
#include "string.h"

#define MAX_TASKS   8
#define STACK_PAGES 4                             /* 16 KiB de stiva per task */
#define STACK_SIZE  (STACK_PAGES * PMM_FRAME_SIZE)
#define USER_STACK_PAGES 4

enum task_state {
    T_UNUSED = 0,
    T_READY,
    T_RUNNING,
    T_SLEEPING,
    T_DYING,
};

struct task {
    uint64_t rsp;          /* cadrul salvat (struct int_frame*) cand nu ruleaza */
    uint64_t stack_base;   /* stiva de kernel (fizic), pentru eliberare */
    uint64_t kstack_top;   /* varful stivei de kernel: ajunge in TSS.rsp0 */
    uint64_t wake_tick;    /* pentru T_SLEEPING */
    address_space_t space; /* PML4-ul cu care ruleaza (CR3) */
    enum task_state state;
    int user;              /* 1 = task ring 3, cu spatiu de adrese propriu */
    int term;              /* terminalul virtual la care scrie/citeste */
    int in_pipe;           /* stdin redirectat dintr-un pipe (-1 = tastatura) */
    int out_pipe;          /* stdout redirectat intr-un pipe (-1 = consola) */
    uint32_t mem_kb;       /* memorie ocupata (KiB): stive + cod user */
    uint32_t disk_kb;      /* dimensiunea programului pe disc (KiB) */
    uint64_t cpu_acc;      /* tick-uri de CPU acumulate (fereastra curenta) */
    uint32_t cpu_pct;      /* procentul de CPU (ultima secunda) */
    char file[24];         /* programul de pe disc, pentru task-uri user */
    char name[16];
};

static struct task tasks[MAX_TASKS];
static int current;
static int sched_enabled;
static uint64_t cpu_window;    /* tick-uri de la ultima resetare a CPU% */

extern char stack_top[];   /* stiva din entry.asm, folosita de task 0 */

/* Sectiune critica scurta: oprim intreruperile si le refacem cum erau. */
static inline uint64_t irq_save(void)
{
    uint64_t fl;
    __asm__ volatile("pushfq; cli; pop %0" : "=r"(fl));
    return fl;
}

static inline void irq_restore(uint64_t fl)
{
    __asm__ volatile("push %0; popfq" : : "r"(fl) : "cc");
}

static void set_name(struct task *t, const char *name)
{
    size_t i = 0;
    for (; name[i] && i < sizeof(t->name) - 1; i++)
        t->name[i] = name[i];
    t->name[i] = '\0';
}

void sched_init(void)
{
    /* Contextul care ruleaza acum (kmain -> shell) devine task-ul 0.
     * rsp-ul lui se va completa la prima comutare. */
    set_name(&tasks[0], "shell");
    tasks[0].state = T_RUNNING;
    tasks[0].kstack_top = (uint64_t)stack_top;
    tasks[0].space = vmm_kernel_space();
    tasks[0].mem_kb = STACK_SIZE / 1024;
    set_name((struct task *)&tasks[0], "shell");   /* deja setat, dar sigur */
    { const char *k = "(kernel)"; int i = 0; for (; k[i]; i++) tasks[0].file[i] = k[i]; tasks[0].file[i] = '\0'; }
    tasks[0].in_pipe = -1;
    tasks[0].out_pipe = -1;
    current = 0;
    sched_enabled = 1;
}

/* Toate task-urile pornesc prin trambulina: daca functia task-ului se
 * intoarce, task-ul moare curat in loc sa sara in gol. */
static void task_trampoline(void (*entry)(void))
{
    entry();
    task_exit();
}

int task_create(const char *name, void (*entry)(void))
{
    uint64_t fl = irq_save();

    int id = -1;
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].state == T_UNUSED) {
            id = i;
            break;
        }
    }
    if (id < 0) {
        irq_restore(fl);
        return -1;
    }

    uint64_t stack = pmm_alloc_contig(STACK_PAGES);
    if (stack == 0) {
        irq_restore(fl);
        return -1;
    }

    /* Construim pe stiva noua un cadru de intrerupere "fals", ca si cum
     * task-ul ar fi fost intrerupt chiar inainte de prima instructiune.
     * Schedulerul il va porni cu un iretq obisnuit. */
    uint64_t top = stack + STACK_SIZE;
    struct int_frame *f = (struct int_frame *)(top - sizeof(struct int_frame));
    memset(f, 0, sizeof(*f));
    f->rdi    = (uint64_t)entry;           /* argumentul trambulinei */
    f->rip    = (uint64_t)task_trampoline;
    f->cs     = 0x08;
    f->rflags = 0x202;                     /* IF=1: task-ul poate fi preemptat */
    f->rsp    = top - 8;                   /* aliniere ABI, ca dupa un call */
    f->ss     = 0x10;

    tasks[id].rsp        = (uint64_t)f;
    tasks[id].stack_base = stack;
    tasks[id].kstack_top = top;
    tasks[id].wake_tick  = 0;
    tasks[id].space      = vmm_kernel_space();
    tasks[id].user       = 0;
    tasks[id].term       = tasks[current].term;
    tasks[id].in_pipe    = -1;
    tasks[id].out_pipe   = -1;
    tasks[id].mem_kb     = STACK_SIZE / 1024;
    tasks[id].disk_kb    = 0;
    tasks[id].cpu_acc    = 0;
    tasks[id].cpu_pct    = 0;
    { const char *k = "(kernel)"; int i = 0; for (; k[i]; i++) tasks[id].file[i] = k[i]; tasks[id].file[i] = '\0'; }
    set_name(&tasks[id], name);
    tasks[id].state      = T_READY;

    irq_restore(fl);
    return id;
}

int task_create_user(const char *name, const void *blob, uint64_t size,
                     const char *args, int term)
{
    uint64_t fl = irq_save();

    int id = -1;
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].state == T_UNUSED) {
            id = i;
            break;
        }
    }
    if (id < 0) {
        irq_restore(fl);
        return -1;
    }

    uint64_t kstack = pmm_alloc_contig(STACK_PAGES);
    if (kstack == 0) {
        irq_restore(fl);
        return -1;
    }

    /* Spatiul de adrese propriu: kernel partajat, user gol. */
    address_space_t space = vmm_create_space();
    if (!space) {
        irq_restore(fl);
        return -1;
    }

    /* Codul programului: fisierele ELF trec prin loader (segmentele merg
     * la adresele lor, .bss ramane zero); orice altceva e tratat ca binar
     * flat copiat la USER_CODE_BASE (programele noastre in asm). */
    uint64_t entry = USER_CODE_BASE;
    const uint8_t *b = blob;

    if (size >= 4 && b[0] == 0x7F && b[1] == 'E' && b[2] == 'L' && b[3] == 'F') {
        if (elf_load(space, blob, size, &entry) < 0) {
            vmm_destroy_space(space);
            for (uint64_t p = 0; p < STACK_PAGES; p++)
                pmm_free(kstack + p * PMM_FRAME_SIZE);
            irq_restore(fl);
            return -1;
        }
    } else {
        uint32_t pages = (uint32_t)((size + PMM_FRAME_SIZE - 1) / PMM_FRAME_SIZE);
        for (uint32_t i = 0; i < pages; i++) {
            uint64_t frame = pmm_alloc();
            if (frame == 0) {
                irq_restore(fl);
                return -1;
            }
            memset((void *)frame, 0, PMM_FRAME_SIZE);
            uint64_t chunk = size - (uint64_t)i * PMM_FRAME_SIZE;
            if (chunk > PMM_FRAME_SIZE)
                chunk = PMM_FRAME_SIZE;
            memcpy((void *)frame, b + (uint64_t)i * PMM_FRAME_SIZE, chunk);
            vmm_map_in(space, USER_CODE_BASE + (uint64_t)i * PMM_FRAME_SIZE,
                       frame, VMM_W | VMM_U);
        }
    }

    /* Stiva user, chiar sub USER_STACK_TOP. */
    for (uint32_t i = 0; i < USER_STACK_PAGES; i++) {
        uint64_t frame = pmm_alloc();
        if (frame == 0) {
            irq_restore(fl);
            return -1;
        }
        memset((void *)frame, 0, PMM_FRAME_SIZE);
        vmm_map_in(space, USER_STACK_TOP - (uint64_t)(i + 1) * PMM_FRAME_SIZE,
                   frame, VMM_W | VMM_U);
    }

    /* Argumentele programului: string NUL-terminat la varful stivei user
     * (in ultimii 256 de bytes); programul il primeste in RDI. Scriem prin
     * adresa fizica — spatiul procesului nu e inca in CR3. */
    uint64_t args_va = USER_STACK_TOP - 256;
    uint64_t top_page = vmm_translate_in(space, USER_STACK_TOP - PMM_FRAME_SIZE);
    char *adst = (char *)(top_page + PMM_FRAME_SIZE - 256);
    uint64_t alen = 0;
    if (args)
        for (; args[alen] && alen < 255; alen++)
            adst[alen] = args[alen];
    adst[alen] = '\0';

    /* Cadrul fals sta pe stiva de KERNEL a task-ului; iretq cu CS/SS de
     * ring 3 face trecerea in user mode. */
    uint64_t top = kstack + STACK_SIZE;
    struct int_frame *f = (struct int_frame *)(top - sizeof(struct int_frame));
    memset(f, 0, sizeof(*f));
    f->rip    = entry;
    f->rdi    = args_va;                /* primul argument al programului */
    f->cs     = 0x1B;                   /* cod user, RPL=3 */
    f->rflags = 0x202;
    f->rsp    = USER_STACK_TOP - 512;   /* sub zona de argumente */
    f->ss     = 0x23;                   /* date user, RPL=3 */

    tasks[id].rsp        = (uint64_t)f;
    tasks[id].stack_base = kstack;
    tasks[id].kstack_top = top;
    tasks[id].wake_tick  = 0;
    tasks[id].space      = space;
    tasks[id].user       = 1;
    tasks[id].term       = (term >= 0 && term < CON_COUNT)
                               ? term : tasks[current].term;
    tasks[id].in_pipe    = -1;
    tasks[id].out_pipe   = -1;
    /* memorie: stiva de kernel + codul + stiva user (16 KiB) */
    tasks[id].mem_kb     = STACK_SIZE / 1024 +
                           (uint32_t)((size + 4095) / 4096) * 4 +
                           USER_STACK_PAGES * 4;
    tasks[id].disk_kb    = (uint32_t)((size + 1023) / 1024);
    tasks[id].cpu_acc    = 0;
    tasks[id].cpu_pct    = 0;
    { int i = 0; for (; name[i] && i < 23; i++) tasks[id].file[i] = name[i]; tasks[id].file[i] = '\0'; }
    set_name(&tasks[id], name);
    tasks[id].state      = T_READY;

    irq_restore(fl);
    return id;
}

void task_yield(void)
{
    __asm__ volatile("int $48");
}

void task_sleep(uint64_t ms)
{
    /* PIT-ul e la 100 Hz => un tick = 10 ms. */
    tasks[current].wake_tick = pit_ticks() + (ms + 9) / 10;
    tasks[current].state = T_SLEEPING;
    task_yield();
}

void task_exit(void)
{
    tasks[current].state = T_DYING;   /* stiva o elibereaza schedulerul */
    for (;;)
        task_yield();
}

void task_kill_current(void)
{
    tasks[current].state = T_DYING;
}

void task_sleep_current(uint64_t ms)
{
    tasks[current].wake_tick = pit_ticks() + (ms + 9) / 10;
    tasks[current].state = T_SLEEPING;
}

int task_current_id(void)
{
    return current;
}

int task_current_term(void)
{
    return tasks[current].term;
}

void task_set_pipes(int id, int in_pipe, int out_pipe)
{
    if (id < 0 || id >= MAX_TASKS)
        return;
    tasks[id].in_pipe = in_pipe;
    tasks[id].out_pipe = out_pipe;
    if (in_pipe >= 0)
        pipe_set_reader(in_pipe, id);
    if (out_pipe >= 0)
        pipe_set_writer(out_pipe, id);
}

int task_current_in_pipe(void)
{
    return tasks[current].in_pipe;
}

int task_current_out_pipe(void)
{
    return tasks[current].out_pipe;
}

int task_alive(int id)
{
    if (id < 0 || id >= MAX_TASKS)
        return 0;
    return tasks[id].state != T_UNUSED;
}

/* Elibereaza tot ce detinea un task mort: spatiul de adrese (cu cadrele
 * user cu tot) si stiva de kernel. Se cheama doar pentru task-uri care NU
 * ruleaza, deci spatiul lor nu e in CR3. */
static void reap(struct task *t)
{
    /* deconectam capetele de pipe: cititorul ramas vede EOF, scriitorul
     * ramas vede "fara cititor" */
    if (t->in_pipe >= 0) {
        pipe_close_reader(t->in_pipe);
        t->in_pipe = -1;
    }
    if (t->out_pipe >= 0) {
        pipe_close_writer(t->out_pipe);
        t->out_pipe = -1;
    }
    if (t->user) {
        vmm_destroy_space(t->space);
        t->user = 0;
    }
    t->space = 0;
    for (uint64_t p = 0; p < STACK_PAGES; p++)
        pmm_free(t->stack_base + p * PMM_FRAME_SIZE);
    t->state = T_UNUSED;
}

uint64_t sched_tick(uint64_t cur_rsp)
{
    if (!sched_enabled)
        return cur_rsp;

    uint64_t now = pit_ticks();

    /* contorizarea CPU: task-ul curent a "consumat" acest tick. La fiecare
     * secunda (100 tick-uri) transformam acumularea in procente. */
    tasks[current].cpu_acc++;
    if (++cpu_window >= 100) {
        for (int i = 0; i < MAX_TASKS; i++) {
            tasks[i].cpu_pct = (uint32_t)(tasks[i].cpu_acc * 100 / cpu_window);
            tasks[i].cpu_acc = 0;
        }
        cpu_window = 0;
    }

    tasks[current].rsp = cur_rsp;
    if (tasks[current].state == T_RUNNING)
        tasks[current].state = T_READY;

    for (int i = 0; i < MAX_TASKS; i++) {
        /* trezim task-urile al caror somn a expirat */
        if (tasks[i].state == T_SLEEPING && tasks[i].wake_tick <= now)
            tasks[i].state = T_READY;
        /* eliberam resursele task-urilor moarte (nu si ale celui curent) */
        if (tasks[i].state == T_DYING && i != current)
            reap(&tasks[i]);
    }

    /* round-robin: primul task READY de dupa cel curent */
    int next = current;
    for (int off = 1; off <= MAX_TASKS; off++) {
        int i = (current + off) % MAX_TASKS;
        if (tasks[i].state == T_READY) {
            next = i;
            break;
        }
    }
    if (tasks[next].state != T_READY)
        return cur_rsp;        /* nimeni pregatit: continua task-ul curent */

    tasks[next].state = T_RUNNING;
    current = next;

    /* Daca task-ul va fi intrerupt din ring 3, CPU-ul trece pe stiva lui
     * de kernel — cea din TSS.rsp0. */
    tss_set_rsp0(tasks[next].kstack_top);

    /* Comutam spatiul de adrese doar daca difera (scrierea in CR3
     * goleste TLB-ul, deci nu o facem degeaba). */
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    if (cr3 != (uint64_t)tasks[next].space)
        __asm__ volatile("mov %0, %%cr3" : : "r"((uint64_t)tasks[next].space)
                         : "memory");

    return tasks[next].rsp;
}

int task_ps_dump(char *out, int max)
{
    static const char *state_name[] = {
        "-", "gata", "ruleaza", "doarme", "moare",
    };
    int pos = 0;

#define PUT(ch) do { if (pos < max) out[pos++] = (ch); } while (0)
#define PUTS(s) do { for (const char *q = (s); *q; q++) PUT(*q); } while (0)

    PUTS("  ID  NUME          STARE    TERM\n");
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].state == T_UNUSED)
            continue;
        PUTS("  ");
        PUT((char)('0' + i));
        PUTS("   ");
        PUTS(tasks[i].name);
        for (size_t p = strlen(tasks[i].name); p < 14; p++)
            PUT(' ');
        PUTS(state_name[tasks[i].state]);
        for (size_t p = strlen(state_name[tasks[i].state]); p < 9; p++)
            PUT(' ');
        PUT('F');
        PUT((char)('1' + tasks[i].term));
        PUT('\n');
    }
#undef PUT
#undef PUTS
    return pos;
}

void task_ps(void)
{
    static char buf[768];
    int n = task_ps_dump(buf, sizeof(buf) - 1);
    buf[n] = '\0';
    kprintf("%s", buf);
}

int task_count_max(void)
{
    return MAX_TASKS;
}

int task_get_info(int id, struct task_info *out)
{
    if (id < 0 || id >= MAX_TASKS || tasks[id].state == T_UNUSED)
        return 0;
    struct task *t = &tasks[id];
    out->used    = 1;
    out->state   = (int)t->state;
    out->user    = t->user;
    out->term    = t->user ? t->term : -1;
    out->mem_kb  = t->mem_kb;
    out->disk_kb = t->disk_kb;
    out->cpu_pct = t->cpu_pct;
    int i = 0;
    for (; t->name[i] && i < 15; i++)
        out->name[i] = t->name[i];
    out->name[i] = '\0';
    for (i = 0; t->file[i] && i < 23; i++)
        out->file[i] = t->file[i];
    out->file[i] = '\0';
    return 1;
}

int task_kill_id(int id)
{
    if (id <= 0 || id >= MAX_TASKS)   /* task 0 = init/shell, nu se omoara */
        return -1;
    if (tasks[id].state == T_UNUSED)
        return -1;
    tasks[id].state = T_DYING;
    return 0;
}
