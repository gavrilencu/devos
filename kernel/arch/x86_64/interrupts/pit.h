#pragma once
#include <stdint.h>

/* Timerul programabil 8253/8254 (PIT) pe IRQ0 — ceasul sistemului. */

void pit_init(uint32_t hz);
uint64_t pit_ticks(void);

/* Functie chemata o data pe secunda, din contextul IRQ-ului de timer —
 * trebuie sa fie scurta si sa nu blocheze. */
void pit_set_second_callback(void (*fn)(uint64_t seconds));

/* Functie chemata des (~12 ori/sec) din IRQ-ul de timer: pentru redesenari
 * rapide care se auto-ignora daca nu e nimic de facut. Scurta, neblocanta. */
void pit_set_fast_callback(void (*fn)(void));
