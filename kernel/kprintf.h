#pragma once

/* Consola kernelului: tot ce se scrie ajunge si pe VGA, si pe serial.
 * Formatare suportata: %c %s %d %u %x %p %% cu modificatorul l
 * si latime cu zero-padding (ex. %016lx). */

void kputc(char c);
void kprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
