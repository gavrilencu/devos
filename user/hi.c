/* hi — program minuscul folosit ca tinta pentru exec() in proctest:
 * afiseaza cine e (PID + argumente) si iese cu un cod cunoscut (7). */

#include <stdint.h>
#include "lib/ulib.h"

int umain(const char *args)
{
    print("  [hi] program nou dupa exec, PID=");
    print_num(getpid());
    print(", args='");
    print(args ? args : "");
    print("'\n");
    return 7;
}
