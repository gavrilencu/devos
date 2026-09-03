/* show — scrie un fisier pe stdout. Spre deosebire de builtin-ul `cat`
 * din ush, e un program adevarat, deci merge in pipeline-uri:
 *   show readme.txt | upper */

#include <stdint.h>
#include "lib/ulib.h"

static char buf[8192];

int umain(const char *args)
{
    char fname[24];
    int i = 0;
    while (args && args[i] && args[i] != ' ' && i < 23) {
        fname[i] = args[i];
        i++;
    }
    fname[i] = '\0';
    if (fname[0] == '\0') {
        print("utilizare: show <fisier>\n");
        return 1;
    }

    int64_t n = fread_file(fname, buf, sizeof(buf));
    if (n < 0) {
        print("nu exista '");
        print(fname);
        print("'\n");
        return 1;
    }
    write_buf(buf, (uint64_t)n);
    return 0;
}
