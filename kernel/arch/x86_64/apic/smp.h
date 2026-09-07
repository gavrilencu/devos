#pragma once

/* SMP (Milestone 57): porneste nucleele secundare (AP) prin trampolina +
 * INIT-SIPI-SIPI si le aduce in long mode. In aceasta faza nucleele sunt
 * "parcate" (halt) dupa pornire — schedulerul multi-core (spinlock-uri,
 * task-uri pe orice nucleu) vine la Milestone 58. */

void smp_init(void);
int  smp_cpu_count(void);   /* cate nuclee au raspuns (inclusiv BSP) */
