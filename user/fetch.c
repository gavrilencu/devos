/* fetch — client HTTP simplu (GET) din MyOS. Descarca o pagina prin TCP
 * si o afiseaza; salveaza corpul in web.html (pentru viitorul browser).
 * Doar HTTP (port 80) - TLS/HTTPS necesita criptografie, inca in lucru.
 *
 *   fetch example.com
 *   fetch example.com /index.html
 *   fetch http://example.com/pagina.html   */

#include <stdint.h>
#include "lib/ulib.h"

static char resp[49152];
static char req[600];

static int starts(const char *s, const char *pre)
{
    while (*pre)
        if (*s++ != *pre++)
            return 0;
    return 1;
}

int umain(const char *args)
{
    char host[160];
    char path[320];
    int port = 80;

    if (!args)
        args = "";
    /* primul token = URL (poate contine schema si calea) */
    char url[400];
    int i = 0, j = 0;
    while (args[i] == ' ')
        i++;
    while (args[i] && args[i] != ' ' && j < 399)
        url[j++] = args[i++];
    url[j] = '\0';
    while (args[i] == ' ')
        i++;

    if (!url[0]) {
        print("folosire: fetch <gazda> [cale]\n");
        return 1;
    }

    const char *u = url;
    if (starts(u, "http://"))
        u += 7;
    else if (starts(u, "https://")) {
        print("HTTPS necesita TLS (criptografie) - inca in lucru.\n");
        print("Incerc totusi pe HTTP simplu...\n");
        u += 8;
    }

    /* desparte gazda de cale la primul '/' */
    j = 0;
    while (*u && *u != '/' && *u != ':' && j < 159)
        host[j++] = *u++;
    host[j] = '\0';
    if (*u == ':') {                     /* port explicit in URL */
        u++;
        int p = 0;
        while (*u >= '0' && *u <= '9')
            p = p * 10 + (*u++ - '0');
        if (p > 0 && p < 65536)
            port = p;
    }
    j = 0;
    if (*u == '/')
        while (*u && j < 319)
            path[j++] = *u++;
    path[j] = '\0';

    /* al doilea token (daca exista) suprascrie calea */
    if (args[i]) {
        j = 0;
        while (args[i] && args[i] != ' ' && j < 319)
            path[j++] = args[i++];
        path[j] = '\0';
    }
    if (!path[0]) {
        path[0] = '/';
        path[1] = '\0';
    }
    if (!host[0]) {
        print("URL invalid.\n");
        return 1;
    }

    print("Rezolv "); print(host); print(" ...\n");
    uint32_t ip = host_resolve(host);
    if (!ip) {
        print("Nu pot rezolva gazda.\n");
        return 1;
    }
    print("GET http://"); print(host); print(path); print(" ...\n");

    int h = tcp_connect(ip, (uint16_t)port);
    if (h < 0) {
        print("Nu sunt conexiuni libere.\n");
        return 1;
    }
    int st;
    while ((st = tcp_status(h)) == 1)
        sleep_ms(30);
    if (st != 2) {
        print("Conectare esuata (timeout sau refuzat).\n");
        return 1;
    }

    /* construim cererea HTTP/1.0 */
    int p = 0;
    const char *pre = "GET ";
    while (*pre) req[p++] = *pre++;
    for (const char *s = path; *s; s++) req[p++] = *s;
    const char *mid = " HTTP/1.0\r\nHost: ";
    while (*mid) req[p++] = *mid++;
    for (const char *s = host; *s; s++) req[p++] = *s;
    const char *end = "\r\nUser-Agent: MyOS/1.0\r\nConnection: close\r\n\r\n";
    while (*end) req[p++] = *end++;
    tcp_send(h, req, p);

    /* citim raspunsul pina la inchidere */
    int total = 0, idle = 0;
    for (;;) {
        int cap = (int)sizeof(resp) - 1 - total;
        int n = cap > 0 ? tcp_recv(h, resp + total, cap) : 0;
        if (n > 0) {
            total += n;
            idle = 0;
            if (total >= (int)sizeof(resp) - 1)
                break;
        } else {
            if (tcp_status(h) == 0) {
                int m = cap > 0 ? tcp_recv(h, resp + total, cap) : 0;
                if (m > 0) { total += m; continue; }
                break;
            }
            if (++idle > 400) { print("[timeout la citire]\n"); break; }
            sleep_ms(15);
        }
    }
    tcp_close(h);
    resp[total] = '\0';

    /* desparte antetele de corp */
    int split = -1;
    for (int k = 0; k + 3 < total; k++)
        if (resp[k] == '\r' && resp[k + 1] == '\n' &&
            resp[k + 2] == '\r' && resp[k + 3] == '\n') {
            split = k + 4;
            break;
        }
    int body = split >= 0 ? split : 0;

    print("--- "); print_num(total); print(" octeti primiti ---\n");
    print(resp + body);
    print_char('\n');

    if (total > body) {
        int64_t w = fwrite_file("web.html", resp + body,
                                (uint64_t)(total - body));
        if (w == 0)
            print("(corpul salvat in web.html)\n");
    }
    return 0;
}
