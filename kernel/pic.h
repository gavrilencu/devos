#pragma once

/* Controllerul de intreruperi 8259A (PIC), in configuratia clasica
 * master + slave. Mai tarziu il vom inlocui cu APIC. */

void pic_init(void);
void pic_send_eoi(int irq);
void pic_unmask(int irq);
