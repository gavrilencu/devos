/* upper — filtru: citeste stdin pana la EOF si scrie totul cu majuscule.
 * Facut pentru pipeline-uri: show fisier | upper */

#include <stdint.h>
#include "lib/ulib.h"

int umain(const char *args)
{
    (void)args;
    int c;
    while ((c = read_char()) >= 0) {
        char ch = (char)c;
        if (ch >= 'a' && ch <= 'z')
            ch = (char)(ch - 32);
        print_char(ch);
    }
    return 0;
}
