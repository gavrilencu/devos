#include <stdarg.h>
#include "klog.h"
#include "kprintf.h"

static klog_level g_max = KLOG_INFO;
static const char *const tag[] = { "EROARE", "WARN ", "INFO ", "DEBUG" };

void klog_set_level(klog_level max)
{
    g_max = max;
}

void klog(klog_level lvl, const char *subsys, const char *fmt, ...)
{
    if (lvl > g_max)
        return;
    kprintf("[%s] %s: ", tag[lvl], subsys);
    va_list ap;
    va_start(ap, fmt);
    vkprintf(fmt, ap);
    va_end(ap);
    kputc('\n');
}

void kernel_panic(const char *fmt, ...)
{
    __asm__ volatile("cli");
    kprintf("\n\n*** PANICA KERNEL DevOS ***\n");
    va_list ap;
    va_start(ap, fmt);
    vkprintf(fmt, ap);
    va_end(ap);
    kprintf("\nSistemul a fost oprit.\n");
    for (;;)
        __asm__ volatile("hlt");
}
