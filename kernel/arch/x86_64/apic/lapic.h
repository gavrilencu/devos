#pragma once
#include <stdint.h>

/* Local APIC (Milestone 56): mapeaza si activeaza APIC-ul local al nucleului
 * curent. Pentru moment intreruperile continua sa fie rutate prin PIC-ul
 * legacy; Local APIC-ul e pregatit pentru SMP (Milestone 57) si pentru
 * timer-ul APIC per-nucleu. */

int      lapic_init(void);     /* 1 = activat, 0 = indisponibil */
int      lapic_present(void);
uint32_t lapic_id(void);       /* APIC ID-ul nucleului curent */
void     lapic_eoi(void);      /* End-Of-Interrupt catre Local APIC */

/* Pornirea unui nucleu secundar (AP): secventa INIT apoi SIPI (Milestone 57). */
void     lapic_send_init(uint32_t apic_id);
void     lapic_send_startup(uint32_t apic_id, uint8_t vector);
