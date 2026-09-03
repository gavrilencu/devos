#include "pic.h"
#include "io.h"

#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xA0
#define PIC2_DATA 0xA1

/* PIC-ul e hardware vechi si lent; o scriere pe portul 0x80 ii da timp
 * sa proceseze comanda precedenta. */
static void io_wait(void)
{
    outb(0x80, 0);
}

void pic_init(void)
{
    /* Din fabrica, IRQ 0-7 vin pe vectorii 8-15, care se suprapun cu
     * exceptiile CPU. Le remapam pe 32-47. */
    outb(PIC1_CMD, 0x11);  io_wait();   /* ICW1: initializare + ICW4 urmeaza */
    outb(PIC2_CMD, 0x11);  io_wait();
    outb(PIC1_DATA, 32);   io_wait();   /* ICW2: master -> vectorii 32-39 */
    outb(PIC2_DATA, 40);   io_wait();   /* ICW2: slave  -> vectorii 40-47 */
    outb(PIC1_DATA, 4);    io_wait();   /* ICW3: slave legat pe IRQ2 */
    outb(PIC2_DATA, 2);    io_wait();
    outb(PIC1_DATA, 0x01); io_wait();   /* ICW4: mod 8086 */
    outb(PIC2_DATA, 0x01); io_wait();

    /* Mascam tot in afara de IRQ0 (PIT), IRQ1 (tastatura), IRQ2 (cascada
     * catre slave — obligatorie pentru orice IRQ 8-15) si IRQ12 (mouse). */
    outb(PIC1_DATA, 0xF8);
    outb(PIC2_DATA, 0xEF);
}

void pic_send_eoi(int irq)
{
    if (irq >= 8)
        outb(PIC2_CMD, 0x20);
    outb(PIC1_CMD, 0x20);
}

/* Deblocheaza (unmask) o linie IRQ, ca sa ajunga la CPU. */
void pic_unmask(int irq)
{
    uint16_t port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    uint8_t bit = (uint8_t)(irq < 8 ? irq : irq - 8);
    uint8_t mask = inb(port);
    mask &= (uint8_t)~(1 << bit);
    outb(port, mask);
}
