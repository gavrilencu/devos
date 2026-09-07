#include "smp.h"
#include "acpi.h"
#include "lapic.h"
#include "pmm.h"
#include "vmm.h"
#include "pit.h"
#include "string.h"
#include "klog.h"
#include "spinlock.h"
#include "interrupts.h"    /* gdt_init, idt_load */

/* Trampolina AP ruleaza la 0x8000 (vector SIPI 0x08). Parametrii sunt scrisi
 * de BSP in pagina trampolinei (vezi ap_trampoline.asm). */
#define TRAMPOLINE_ADDR  0x8000
#define TRAMPOLINE_VEC   0x08
#define PARAM_CR3        0x8FF0
#define PARAM_STACK      0x8FE0
#define PARAM_ENTRY      0x8FD0

#define AP_STACK_PAGES   4

/* codul trampolinei, inglobat ca binar de Makefile (objcopy) */
extern char _binary_ap_trampoline_bin_start[];
extern char _binary_ap_trampoline_bin_end[];

static volatile int ap_online;     /* setat de AP-ul curent cand ajunge sus */
static int cpus_up = 1;            /* BSP e deja pornit */

/* --- test de paralelism (Milestone 58): toate nucleele incrementeaza un
 * contor partajat sub spinlock. Daca lock-ul e corect, contorul final e EXACT
 * (nicio actualizare pierduta), oricat de tare s-ar bate nucleele pe el. --- */
#define WORK_ITERS 50000
static spinlock_t       work_lock = SPINLOCK_INIT;
static volatile uint64_t work_counter;
static volatile int      work_go;
static volatile int      work_done;
static uint32_t          work_contrib[ACPI_MAX_CPUS];

/* Bataile de timer per-nucleu: dovada ca fiecare nucleu ruleaza cod de kernel
 * declansat de propriul timer LAPIC (substratul scheduler-ului per-nucleu). */
static volatile uint64_t cpu_heartbeat[ACPI_MAX_CPUS];

void smp_cpu_tick(void)
{
    uint32_t id = lapic_id();
    if (id < ACPI_MAX_CPUS)
        cpu_heartbeat[id]++;
}

/* --- Scheduler per-nucleu pentru task-uri de kernel pe AP-uri (Milestone 58,
 * SMP scheduler) ---
 *
 * Fiecare nucleu secundar isi tine PROPRIA coada de task-uri de kernel (ring 0)
 * si le comuta round-robin la fiecare tick al PROPRIULUI timer LAPIC. Nu atinge
 * scheduler-ul BSP-ului (PIT + tabela globala `tasks[]` din sched/task.c), care
 * ruleaza in continuare GUI-ul/shell-urile/browser-ul — deci restul sistemului
 * ramane neschimbat si stabil. Task-urile AP ruleaza in ring 0, deci NU au
 * nevoie de TSS (nicio schimbare de privilegiu la intrerupere); comutarea de
 * context foloseste acelasi mecanism ca la BSP: un cadru de intrerupere fals pe
 * stiva fiecarui task, iar iretq-ul din stub-ul ISR il porneste. */
#define AP_MAX_TASKS  4

struct ap_task {
    uint64_t rsp;          /* cadrul salvat cand task-ul nu ruleaza */
    uint64_t stack_base;   /* stiva de kernel (fizic) */
    volatile int used;     /* 1 = slot ocupat (publicat cu RELEASE) */
    char name[16];
};

struct ap_cpu {
    struct ap_task tasks[AP_MAX_TASKS];
    int      current;      /* index in tasks[], -1 = idle */
    uint64_t idle_rsp;     /* contextul buclei idle a nucleului */
    volatile int active;   /* 1 = acest nucleu ruleaza scheduler-ul AP */
};

static struct ap_cpu apcpu[ACPI_MAX_CPUS];

/* Trambulina task-urilor AP: daca functia se intoarce, nucleul idle-uieste
 * (task-ul nu mai e reprogramat, dar sistemul merge mai departe). */
static void ap_task_trampoline(void (*entry)(void))
{
    entry();
    for (;;)
        __asm__ volatile("hlt");
}

/* Creeaza un task de kernel legat (pinned) de nucleul `cpu`. Se cheama de pe
 * BSP; publicarea slotului (used=1) e cu RELEASE, iar tick-ul AP-ului il vede
 * cu ACQUIRE — deci nu e nevoie de lock intre BSP si tick. */
int ap_sched_spawn(uint32_t cpu, const char *name, void (*entry)(void))
{
    if (cpu >= ACPI_MAX_CPUS)
        return -1;
    struct ap_cpu *c = &apcpu[cpu];

    int slot = -1;
    for (int i = 0; i < AP_MAX_TASKS; i++)
        if (!c->tasks[i].used) { slot = i; break; }
    if (slot < 0)
        return -1;

    uint64_t stack = pmm_alloc_contig(AP_STACK_PAGES);
    if (stack == 0)
        return -1;
    uint64_t top = stack + AP_STACK_PAGES * PMM_FRAME_SIZE;

    struct int_frame *f = (struct int_frame *)(top - sizeof(struct int_frame));
    memset(f, 0, sizeof(*f));
    f->rdi    = (uint64_t)entry;            /* argumentul trambulinei */
    f->rip    = (uint64_t)ap_task_trampoline;
    f->cs     = 0x08;                       /* cod kernel 64-bit */
    f->rflags = 0x202;                      /* IF=1: preemptabil */
    f->rsp    = top - 8;
    f->ss     = 0x10;                       /* date kernel */

    struct ap_task *t = &c->tasks[slot];
    t->rsp        = (uint64_t)f;
    t->stack_base = stack;
    { int i = 0; for (; name[i] && i < 15; i++) t->name[i] = name[i]; t->name[i] = '\0'; }
    __atomic_store_n(&t->used, 1, __ATOMIC_RELEASE);   /* publica slotul */
    return slot;
}

/* Tick-ul scheduler-ului AP: apelat din handler-ul timer-ului LAPIC (vector
 * 0x40) pentru FIECARE nucleu. Pe BSP si pe nucleele fara scheduler AP intoarce
 * exact cadrul primit (nicio comutare). Pe un AP activ, salveaza contextul
 * curent si intoarce cadrul urmatorului task (round-robin). */
uint64_t ap_sched_tick(uint64_t cur_rsp)
{
    uint32_t id = lapic_id();
    if (id >= ACPI_MAX_CPUS)
        return cur_rsp;
    struct ap_cpu *c = &apcpu[id];
    if (!c->active)
        return cur_rsp;                     /* BSP / nucleu fara task-uri AP */

    /* salveaza contextul care tocmai a rulat */
    if (c->current < 0)
        c->idle_rsp = cur_rsp;
    else
        c->tasks[c->current].rsp = cur_rsp;

    /* round-robin: primul slot ocupat de dupa cel curent */
    int base = (c->current < 0) ? -1 : c->current;
    int next = -1;
    for (int off = 1; off <= AP_MAX_TASKS; off++) {
        int i = (base + off + AP_MAX_TASKS) % AP_MAX_TASKS;
        if (__atomic_load_n(&c->tasks[i].used, __ATOMIC_ACQUIRE)) { next = i; break; }
    }
    if (next < 0) {                         /* niciun task: ramanem idle */
        c->current = -1;
        return c->idle_rsp;
    }
    c->current = next;
    return c->tasks[next].rsp;
}

/* Porneste scheduler-ul AP pe nucleul curent (chemat din ap_entry). */
static void ap_sched_enable(uint32_t id)
{
    if (id >= ACPI_MAX_CPUS)
        return;
    apcpu[id].current = -1;
    __asm__ volatile("mfence" ::: "memory");
    apcpu[id].active = 1;
}

/* Doua task-uri de kernel de demonstratie: fac muncа reala (incrementeaza un
 * contor propriu, per-nucleu) ca sa se poata masura din BSP ca AMBELE ruleaza
 * pe acel nucleu — dovada ca schedulerul lui comuta intre doua task-uri. */
static volatile uint64_t ap_work_a[ACPI_MAX_CPUS];
static volatile uint64_t ap_work_b[ACPI_MAX_CPUS];

static void ap_worker_a(void)
{
    for (;;) {
        uint32_t id = lapic_id();
        if (id < ACPI_MAX_CPUS)
            ap_work_a[id]++;
        for (volatile int i = 0; i < 3000; i++)   /* munca */
            __asm__ volatile("");
    }
}

static void ap_worker_b(void)
{
    for (;;) {
        uint32_t id = lapic_id();
        if (id < ACPI_MAX_CPUS)
            ap_work_b[id]++;
        for (volatile int i = 0; i < 3000; i++)
            __asm__ volatile("");
    }
}

static void run_workload(uint32_t apic_id)
{
    for (int i = 0; i < WORK_ITERS; i++) {
        spin_lock(&work_lock);
        work_counter++;
        spin_unlock(&work_lock);
    }
    if (apic_id < ACPI_MAX_CPUS)
        work_contrib[apic_id] = WORK_ITERS;
    __atomic_add_fetch(&work_done, 1, __ATOMIC_SEQ_CST);
}

/* Punctul de intrare 64-bit al fiecarui AP (apelat de trampolina). */
static void ap_entry(void)
{
    lapic_init();                  /* activeaza Local APIC-ul acestui nucleu */
    uint32_t id = lapic_id();
    __asm__ volatile("mfence" ::: "memory");
    ap_online = 1;                 /* semnalam BSP-ului ca suntem sus */

    while (!work_go)               /* asteptam startul testului paralel */
        __asm__ volatile("pause");
    run_workload(id);              /* rulam concurent cu celelalte nuclee */

    /* Devenim un nucleu VIU: incarcam GDT-ul + IDT-ul kernelului (ca sa putem
     * primi intreruperi cu selectori corecti), pornim timer-ul LAPIC propriu
     * si activam intreruperile. De acum, la fiecare tick, nucleul ruleaza
     * codul de kernel (smp_cpu_tick) — substratul scheduler-ului per-nucleu. */
    gdt_init();                    /* GDT-ul kernelului (CS 64-bit corect) */
    idt_load();                    /* IDT-ul partajat */
    ap_sched_enable(id);           /* activeaza scheduler-ul AP al acestui nucleu */
    lapic_timer_start(0x40, 100);  /* timer-ul acestui nucleu (tick de scheduler) */
    __asm__ volatile("sti");
    for (;;)                       /* idle: ruleaza cand nu are task-uri gata */
        __asm__ volatile("hlt");
}

/* asteptare ~ms folosind PIT-ul (100 Hz = 10 ms/tick) */
static void wait_ms(int ms)
{
    uint64_t start = pit_ticks();
    while ((int)((pit_ticks() - start) * 10) < ms)
        __asm__ volatile("pause");
}

static void wait_short(void)       /* ~cateva sute de microsecunde (SIPI) */
{
    for (volatile int i = 0; i < 300000; i++)
        __asm__ volatile("");
}

void smp_init(void)
{
    if (!lapic_present()) {
        KINFO("smp", "fara Local APIC");
        return;
    }
    /* Timer-ul LAPIC al nucleului de boot (tick-ul lui propriu). */
    lapic_timer_start(0x40, 100);

    int n = acpi_cpu_count();
    if (n <= 1) {
        KINFO("smp", "un singur nucleu (timer LAPIC activ)");
        return;
    }

    /* copiaza trampolina la 0x8000 si pune parametrii comuni */
    uint64_t sz = (uint64_t)(_binary_ap_trampoline_bin_end -
                             _binary_ap_trampoline_bin_start);
    memcpy((void *)TRAMPOLINE_ADDR, _binary_ap_trampoline_bin_start, sz);
    *(volatile uint64_t *)PARAM_CR3   = (uint64_t)vmm_kernel_space();
    *(volatile uint64_t *)PARAM_ENTRY = (uint64_t)&ap_entry;

    uint32_t bsp = lapic_id();
    for (int i = 0; i < n; i++) {
        uint32_t id = acpi_cpu_apic_id(i);
        if (id == bsp)
            continue;

        uint64_t stk = pmm_alloc_contig(AP_STACK_PAGES);
        if (stk == 0) {
            KERR("smp", "fara memorie pentru stiva nucleului APIC %u", (unsigned)id);
            continue;
        }
        *(volatile uint64_t *)PARAM_STACK = stk + AP_STACK_PAGES * PMM_FRAME_SIZE;

        ap_online = 0;
        __asm__ volatile("mfence" ::: "memory");

        /* secventa standard INIT - SIPI - SIPI */
        lapic_send_init(id);
        wait_ms(10);
        lapic_send_startup(id, TRAMPOLINE_VEC);
        wait_short();
        lapic_send_startup(id, TRAMPOLINE_VEC);

        /* asteptam pana la ~200 ms sa dea semn de viata */
        uint64_t t0 = pit_ticks();
        while (!ap_online && (int)((pit_ticks() - t0) * 10) < 200)
            __asm__ volatile("pause");

        if (ap_online) {
            cpus_up++;
            KINFO("smp", "nucleul APIC ID %u a pornit (parcat)", (unsigned)id);
        } else {
            KWARN("smp", "nucleul APIC ID %u nu a raspuns", (unsigned)id);
            pmm_free(stk);   /* nu a folosit stiva */
        }
    }

    KINFO("smp", "%d din %d nuclee active", cpus_up, n);

    /* Test de paralelism real: toate nucleele (BSP + AP) bat pe acelasi
     * contor sub spinlock. Daca lock-ul e corect, rezultatul e EXACT. */
    work_go = 1;
    __asm__ volatile("mfence" ::: "memory");
    run_workload(bsp);                 /* si BSP-ul participa */

    uint64_t t0 = pit_ticks();
    while (work_done < cpus_up && (int)((pit_ticks() - t0) * 10) < 5000)
        __asm__ volatile("pause");

    uint64_t expected = (uint64_t)cpus_up * WORK_ITERS;
    if (work_done >= cpus_up && work_counter == expected)
        KINFO("smp", "test spinlock OK: %d nuclee, %llu incrementari partajate, "
              "contor = %llu (fara pierderi)", cpus_up,
              (unsigned long long)expected, (unsigned long long)work_counter);
    else
        KERR("smp", "test spinlock ESUAT: contor %llu vs asteptat %llu (done %d/%d)",
             (unsigned long long)work_counter, (unsigned long long)expected,
             work_done, cpus_up);

    /* Milestone 58 (SMP scheduler): pornim cate DOUA task-uri de kernel pe
     * fiecare nucleu secundar. Fiecare nucleu le comuta round-robin din propriul
     * timer LAPIC — deci schedulerul ruleaza pe TOATE nucleele in paralel
     * (CPU1->task, CPU2->task, ...), nu doar pe BSP. */
    for (int i = 0; i < n; i++) {
        uint32_t id = acpi_cpu_apic_id(i);
        if (id == bsp)
            continue;                       /* BSP ruleaza scheduler-ul principal */
        ap_sched_spawn(id, "worker-a", ap_worker_a);
        ap_sched_spawn(id, "worker-b", ap_worker_b);
    }

    /* Lasam nucleele sa lucreze ~500 ms, apoi masuram progresul fiecarui task
     * si cate tick-uri de timer a batut fiecare nucleu. */
    uint64_t base[ACPI_MAX_CPUS];
    for (int i = 0; i < n; i++)
        base[i] = cpu_heartbeat[acpi_cpu_apic_id(i)];
    uint64_t hs = pit_ticks();
    while (pit_ticks() - hs < 50)           /* ~500 ms */
        __asm__ volatile("pause");

    int running = 0;
    for (int i = 0; i < n; i++) {
        uint32_t id = acpi_cpu_apic_id(i);
        if (id == bsp) {
            KINFO("smp", "nucleul %u (BSP): %llu tick-uri LAPIC; ruleaza scheduler-ul principal (PIT)",
                  (unsigned)id, (unsigned long long)(cpu_heartbeat[id] - base[i]));
        } else {
            KINFO("smp", "nucleul %u: %llu tick-uri; task-uri kernel worker-a=%llu worker-b=%llu iteratii",
                  (unsigned)id, (unsigned long long)(cpu_heartbeat[id] - base[i]),
                  (unsigned long long)ap_work_a[id], (unsigned long long)ap_work_b[id]);
            if (ap_work_a[id] > 0 && ap_work_b[id] > 0)
                running++;                  /* ambele task-uri au progresat */
        }
    }
    KINFO("smp", "scheduler multi-core activ: %d nuclee secundare ruleaza cate 2 task-uri de kernel concurent",
          running);
}

int smp_cpu_count(void)
{
    return cpus_up;
}
