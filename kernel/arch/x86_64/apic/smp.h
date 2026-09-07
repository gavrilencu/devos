#pragma once
#include <stdint.h>

/* SMP (Milestone 57): porneste nucleele secundare (AP) prin trampolina +
 * INIT-SIPI-SIPI si le aduce in long mode. In aceasta faza nucleele sunt
 * "parcate" (halt) dupa pornire — schedulerul multi-core (spinlock-uri,
 * task-uri pe orice nucleu) vine la Milestone 58. */

void smp_init(void);
int  smp_cpu_count(void);   /* cate nuclee au raspuns (inclusiv BSP) */
void smp_cpu_tick(void);    /* apelat din handler-ul de timer LAPIC (vector 0x40) */

/* Scheduler per-nucleu (Milestone 58, SMP scheduler): task-uri de kernel legate
 * de un anumit nucleu secundar, comutate round-robin din timer-ul lui LAPIC. */
int      ap_sched_spawn(uint32_t cpu, const char *name, void (*entry)(void));
uint64_t ap_sched_tick(uint64_t cur_rsp);   /* comuta contextul pe nucleul curent */
