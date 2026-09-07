#pragma once

/* Driver pentru portul serial COM1 — util pentru debugging:
 * QEMU poate redirecta iesirea seriala in terminal sau intr-un fisier. */

void serial_init(void);
void serial_putc(char c);
void serial_write(const char *s);
