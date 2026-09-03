/* telnet — client TCP interactiv din MyOS. Se conecteaza la o gazda:port,
 * trimite tastele si afiseaza ce primeste. Iesire: Ctrl+Q.
 * Gestioneaza minimal negocierea telnet (IAC): refuza toate optiunile. */

#include <stdint.h>
#include "lib/ulib.h"

#define IAC  255
#define WILL 251
#define WONT 252
#define DO   253
#define DONT 254

static void print_ip(uint32_t ip)
{
    print_num((ip >> 24) & 255); print_char('.');
    print_num((ip >> 16) & 255); print_char('.');
    print_num((ip >> 8) & 255);  print_char('.');
    print_num(ip & 255);
}

int umain(const char *args)
{
    char host[128];
    int port = 23;

    if (!args)
        args = "";
    int i = 0, j = 0;
    while (args[i] == ' ')
        i++;
    while (args[i] && args[i] != ' ' && j < 127)
        host[j++] = args[i++];
    host[j] = '\0';
    while (args[i] == ' ')
        i++;
    if (args[i] >= '0' && args[i] <= '9') {
        int p = 0;
        while (args[i] >= '0' && args[i] <= '9')
            p = p * 10 + (args[i++] - '0');
        if (p > 0 && p < 65536)
            port = p;
    }
    if (!host[0]) {
        print("folosire: telnet <gazda> [port]\n");
        return 1;
    }

    print("Rezolv "); print(host); print(" ...\n");
    uint32_t ip = host_resolve(host);
    if (!ip) {
        print("Nu pot rezolva gazda.\n");
        return 1;
    }
    print("Conectare la "); print_ip(ip);
    print_char(':'); print_num(port); print(" ...\n");

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
    print("Conectat. Apasa Ctrl+Q pentru a inchide.\n\n");

    uint8_t rbuf[1024];
    int iac = 0, cmd = 0;
    for (;;) {
        int n = tcp_recv(h, rbuf, sizeof(rbuf));
        if (n > 0) {
            for (int k = 0; k < n; k++) {
                uint8_t b = rbuf[k];
                if (iac == 0) {
                    if (b == IAC)
                        iac = 1;
                    else
                        print_char((char)b);
                } else if (iac == 1) {
                    if (b == IAC) {           /* IAC IAC = octetul 255 */
                        print_char((char)0xFF);
                        iac = 0;
                    } else {
                        cmd = b;
                        iac = 2;
                    }
                } else {                       /* iac == 2: optiunea */
                    if (cmd == DO || cmd == WILL) {
                        uint8_t resp[3] = { IAC,
                            (uint8_t)(cmd == DO ? WONT : DONT), b };
                        tcp_send(h, resp, 3);
                    }
                    iac = 0;
                }
            }
        }

        int c = readc();
        if (c == 0x11) {                       /* Ctrl+Q */
            print("\n[inchis]\n");
            tcp_close(h);
            return 0;
        }
        if (c >= 0) {
            if (c == '\n') {
                char crlf[2] = { '\r', '\n' };
                tcp_send(h, crlf, 2);
            } else if (c == '\b') {
                char bs = (char)0x7F;
                tcp_send(h, &bs, 1);
            } else if (c > 0 && c < 0x80) {
                char ch = (char)c;
                tcp_send(h, &ch, 1);
            }
        }

        if (tcp_status(h) == 0) {
            int m = tcp_recv(h, rbuf, sizeof(rbuf));
            if (m > 0) {
                for (int k = 0; k < m; k++)
                    if (rbuf[k] != IAC)
                        print_char((char)rbuf[k]);
            } else {
                print("\n[conexiune inchisa de gazda]\n");
                return 0;
            }
        }
        if (n <= 0)
            sleep_ms(15);
    }
}
