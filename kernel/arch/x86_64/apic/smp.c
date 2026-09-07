#include "smp.h"
#include "acpi.h"
#include "lapic.h"
#include "pmm.h"
#include "vmm.h"
#include "pit.h"
#include "string.h"
#include "klog.h"
#include "spinlock.h"

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

    /* Faza 1: dupa test, parcam nucleul. M58+ il va baga in scheduler. */
    for (;;)
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
    int n = acpi_cpu_count();
    if (n <= 1 || !lapic_present()) {
        KINFO("smp", "un singur nucleu (SMP inactiv)");
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
}

int smp_cpu_count(void)
{
    return cpus_up;
}
