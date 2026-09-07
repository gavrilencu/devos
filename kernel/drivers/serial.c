#include "serial.h"
#include "io.h"

#define COM1 0x3F8

void serial_init(void)
{
    outb(COM1 + 1, 0x00);   /* fara intreruperi */
    outb(COM1 + 3, 0x80);   /* DLAB=1 ca sa setam divizorul */
    outb(COM1 + 0, 0x01);   /* divizor 1 => 115200 baud */
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);   /* 8 biti, fara paritate, 1 bit stop */
    outb(COM1 + 2, 0xC7);   /* FIFO activat si golit */
    outb(COM1 + 4, 0x0B);   /* DTR + RTS + OUT2 */
}

void serial_putc(char c)
{
    /* asteptam sa se goleasca registrul de transmisie */
    while ((inb(COM1 + 5) & 0x20) == 0)
        ;
    outb(COM1, (uint8_t)c);
}

void serial_write(const char *s)
{
    while (*s) {
        if (*s == '\n')
            serial_putc('\r');
        serial_putc(*s++);
    }
}
