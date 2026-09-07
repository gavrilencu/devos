#pragma once

/* Logging pe niveluri pentru kernel (Milestone 53). Fiecare mesaj are un nivel
 * si un nume de subsistem, ex.  KINFO("net", "IP %u.%u.%u.%u", ...).
 * Iesirea merge pe consola (VGA + serial), ca restul kprintf-ului. */

typedef enum {
    KLOG_ERROR = 0,   /* eroare: ceva a esuat */
    KLOG_WARN  = 1,   /* avertisment: situatie neasteptata, dar recuperabila */
    KLOG_INFO  = 2,   /* informativ: pasi normali de functionare */
    KLOG_DEBUG = 3,   /* detalii de depanare */
} klog_level;

/* Nu se afiseaza mesajele cu nivel mai mare decat pragul (implicit KLOG_INFO,
 * deci DEBUG e ascuns). */
void klog_set_level(klog_level max);

void klog(klog_level lvl, const char *subsys, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

#define KERR(sub, ...)  klog(KLOG_ERROR, (sub), __VA_ARGS__)
#define KWARN(sub, ...) klog(KLOG_WARN,  (sub), __VA_ARGS__)
#define KINFO(sub, ...) klog(KLOG_INFO,  (sub), __VA_ARGS__)
#define KDBG(sub, ...)  klog(KLOG_DEBUG, (sub), __VA_ARGS__)

/* Panica centralizata: opreste intreruperile, afiseaza mesajul si blocheaza
 * sistemul. Nu se intoarce niciodata. */
void kernel_panic(const char *fmt, ...)
    __attribute__((format(printf, 1, 2), noreturn));
