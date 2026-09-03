/* lines — filtru: numara liniile si bytes de pe stdin (pana la EOF).
 * Ex: show readme.txt | lines */

#include <stdint.h>
#include "lib/ulib.h"

int umain(const char *args)
{
    (void)args;
    int64_t nlines = 0, nbytes = 0;
    int c;
    while ((c = read_char()) >= 0) {
        nbytes++;
        if (c == '\n')
            nlines++;
    }
    print_num(nlines);
    print(" linii, ");
    print_num(nbytes);
    print(" bytes\n");
    return 0;
}
