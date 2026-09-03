#pragma once
#include <stdint.h>

/* Multitasking preemptiv cu scheduler round-robin.
 * Comutarea de context se face pe stiva: stub-urile ISR salveaza toate
 * registrele, iar schedulerul doar schimba RSP-ul cu care se face iretq. */

void sched_init(void);   /* transforma contextul curent in task-ul 0 ("shell") */

/* Layoutul spatiului user: la 512 GiB, adica in slotul 1 al PML4 —
 * separat de slotul 0 al kernelului, ca fiecare proces sa-si poata avea
 * propriul subtree user in timp ce kernelul e partajat. */
#define USER_CODE_BASE  0x8000000000ull        /* 512 GiB */
#define USER_STACK_TOP  0x8000800000ull        /* 512 GiB + 8 MiB */

/* Creeaza un task de kernel cu stiva proprie de 16 KiB.
 * Intoarce id-ul task-ului sau -1 daca nu mai e loc. */
int task_create(const char *name, void (*entry)(void));

/* Creeaza un task user (ring 3) cu propriul spatiu de adrese: incarca
 * `blob` (ELF sau binar flat) in pagini cu bitul U, ii da stiva user si
 * stiva de kernel si il porneste cu CS/SS de ring 3. `args` (poate fi
 * NULL/gol) e copiat la varful stivei user si pasat programului in RDI.
 * Pot rula mai multe task-uri user simultan — fiecare cu tabelele lui.
 * Intoarce id-ul sau -1 la lipsa resurselor. */
/* `term`: terminalul virtual al task-ului (0..CON_COUNT-1);
 * -1 = mosteneste terminalul task-ului curent. */
int task_create_user(const char *name, const void *blob, uint64_t size,
                     const char *args, int term);

/* Variante apelabile din context de intrerupere/syscall: doar seteaza
 * starea; comutarea o face apelantul, intorcand sched_tick(). */
void task_kill_current(void);
void task_sleep_current(uint64_t ms);

void task_yield(void);            /* cedeaza CPU-ul voluntar (int 48) */
int task_alive(int id);           /* 1 daca slotul e ocupat de un task viu */
void task_sleep(uint64_t ms);     /* doarme cel putin `ms` milisecunde */
void task_exit(void);             /* termina task-ul curent */
int task_current_id(void);
int task_current_term(void);      /* terminalul task-ului curent */

/* Redirectarea I/O prin pipe-uri (vezi pipe.h); -1 = consola/tastatura. */
void task_set_pipes(int id, int in_pipe, int out_pipe);
int task_current_in_pipe(void);
int task_current_out_pipe(void);

void task_ps(void);               /* afiseaza tabela de task-uri (comanda ps) */
int task_ps_dump(char *out, int max);   /* aceeasi tabela, intr-un buffer */

/* --- pentru Task Manager --- */
struct task_info {
    int used;
    int state;         /* 0=liber 1=gata 2=ruleaza 3=doarme 4=moare */
    int user;          /* 1 = proces ring 3 */
    int term;          /* terminalul (-1 daca nu e legat) */
    uint32_t mem_kb;
    uint32_t disk_kb;
    uint32_t cpu_pct;
    char name[16];
    char file[24];     /* programul pe disc, sau "(kernel)" */
};

int task_count_max(void);                        /* MAX_TASKS */
int task_get_info(int id, struct task_info *out);/* 1 daca slotul e ocupat */
int task_kill_id(int id);                        /* 0 ok, -1 nu se poate */

/* Chemat din isr_dispatch la tick de timer sau la yield: salveaza cadrul
 * curent si intoarce cadrul task-ului ales sa ruleze. */
uint64_t sched_tick(uint64_t cur_rsp);
