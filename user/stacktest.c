/* stacktest — verifica cresterea stivei la cerere (demand paging) si zona de
 * garda (guard page). Recurseaza la infinit, fiecare cadru foloseste ~1 KiB;
 * stiva creste de la 16 KiB in sus, mapand pagini pe masura ce e nevoie, pana
 * loveste zona de garda (~2 MiB) — atunci kernelul opreste doar acest proces
 * ("stack overflow"), iar sistemul merge mai departe. */

#include <stdint.h>
#include "lib/ulib.h"

static volatile int sink;

static int recurse(int n)
{
    volatile char buf[1024];        /* ~1 KiB pe cadru -> forteaza crestere */
    buf[0] = (char)n;
    buf[1023] = (char)n;
    if ((n % 64) == 0) {
        print("  nivel ");
        print_num(n);
        print("  (~");
        print_num(n);
        print(" KiB stiva)\n");
    }
    int r = recurse(n + 1);         /* recursie infinita */
    /* folosim buf DUPA apel ca sa nu poata fi optimizat in tail-call (loop) */
    sink += buf[0] + buf[1023] + r;
    return r + 1;
}

int umain(const char *args)
{
    (void)args;
    print("Test crestere stiva la cerere (demand paging + guard page).\n");
    print("Recursez pana la overflow; sistemul trebuie sa ramana viu.\n");
    recurse(1);
    return 0;                       /* nu se ajunge aici */
}
