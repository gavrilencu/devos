#include <stdarg.h>
#include <stdint.h>
#include "kprintf.h"
#include "vga.h"
#include "serial.h"

void kputc(char c)
{
    con_putc(c);   /* pe terminalul task-ului curent */
    if (c == '\n')
        serial_putc('\r');
    serial_putc(c);
}

static void kputs(const char *s)
{
    while (*s)
        kputc(*s++);
}

static void print_u(uint64_t v, unsigned base, int width, char pad)
{
    static const char digits[] = "0123456789abcdef";
    char buf[24];
    int i = 0;

    do {
        buf[i++] = digits[v % base];
        v /= base;
    } while (v);

    while (i < width && i < (int)sizeof(buf))
        buf[i++] = pad;

    while (i--)
        kputc(buf[i]);
}

void kprintf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);

    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            kputc(*fmt);
            continue;
        }
        fmt++;

        char pad = ' ';
        int width = 0;
        int lng = 0;

        if (*fmt == '0') {
            pad = '0';
            fmt++;
        }
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            fmt++;
        }
        while (*fmt == 'l') {
            lng = 1;
            fmt++;
        }

        switch (*fmt) {
        case 'c':
            kputc((char)va_arg(ap, int));
            break;
        case 's': {
            const char *s = va_arg(ap, const char *);
            kputs(s ? s : "(null)");
            break;
        }
        case 'd': {
            int64_t v = lng ? va_arg(ap, int64_t) : va_arg(ap, int);
            if (v < 0) {
                kputc('-');
                v = -v;
            }
            print_u((uint64_t)v, 10, width, pad);
            break;
        }
        case 'u':
            print_u(lng ? va_arg(ap, uint64_t) : va_arg(ap, unsigned), 10, width, pad);
            break;
        case 'x':
            print_u(lng ? va_arg(ap, uint64_t) : va_arg(ap, unsigned), 16, width, pad);
            break;
        case 'p':
            kputs("0x");
            print_u((uint64_t)va_arg(ap, void *), 16, 16, '0');
            break;
        case '%':
            kputc('%');
            break;
        case '\0':
            va_end(ap);
            return;
        default:
            kputc('%');
            kputc(*fmt);
            break;
        }
    }

    va_end(ap);
}
