/* nslookup — rezolva un nume de gazda prin DNS (serverul 10.0.2.3).
 * Foloseste clientul DNS din kernel (UDP port 53). */

#include <stdint.h>
#include "lib/ulib.h"

static void print_ip(uint32_t ip)
{
    print_num((ip >> 24) & 255); print_char('.');
    print_num((ip >> 16) & 255); print_char('.');
    print_num((ip >> 8) & 255);  print_char('.');
    print_num(ip & 255);
}

int umain(const char *args)
{
    if (!args || !args[0]) {
        print("folosire: nslookup <nume>\n");
        return 1;
    }
    print("Rezolv "); print(args); print(" ...\n");
    uint32_t ip = dns_resolve(args);
    if (!ip) {
        print("Esuat: nume necunoscut sau fara retea.\n");
        return 1;
    }
    print("Adresa: ");
    print_ip(ip);
    print_char('\n');
    return 0;
}
