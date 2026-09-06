/* ssh — client SSH interactiv din DevOS. Se conecteaza la o gazda, se
 * autentifica cu parola si deschide un shell securizat. Criptografia
 * (curve25519 + AES-CTR + HMAC-SHA256) ruleaza in kernel. Iesire: Ctrl+Q.
 * Folosire: ssh <gazda> [utilizator] [port] */

#include <stdint.h>
#include "lib/ulib.h"

static void print_ip(uint32_t ip)
{
    print_num((ip >> 24) & 255); print_char('.');
    print_num((ip >> 16) & 255); print_char('.');
    print_num((ip >> 8) & 255);  print_char('.');
    print_num(ip & 255);
}

/* citeste o linie. echo=0 mascat cu '*', echo=1 vizibil (pentru verificare) */
static void read_line_echo(char *buf, int max, int echo)
{
    int n = 0;
    for (;;) {
        int c = readc();
        if (c < 0) { sleep_ms(15); continue; }
        if (c == '\n' || c == '\r') break;
        if (c == '\b') { if (n > 0) { n--; print("\b \b"); } continue; }
        if (c >= 32 && c < 127 && n < max - 1) {
            buf[n++] = (char)c;
            print_char(echo ? (char)c : '*');
        }
    }
    buf[n] = '\0';
    print_char('\n');
}

int umain(const char *args)
{
    char host[128], user[64];
    int port = 22;

    if (!args) args = "";
    int i = 0, j = 0;
    while (args[i] == ' ') i++;
    while (args[i] && args[i] != ' ' && j < 127) host[j++] = args[i++];
    host[j] = '\0';
    /* utilizator (optional) */
    while (args[i] == ' ') i++;
    j = 0;
    while (args[i] && args[i] != ' ' && j < 63) user[j++] = args[i++];
    user[j] = '\0';
    /* port (optional) */
    while (args[i] == ' ') i++;
    if (args[i] >= '0' && args[i] <= '9') {
        int p = 0;
        while (args[i] >= '0' && args[i] <= '9') p = p * 10 + (args[i++] - '0');
        if (p > 0 && p < 65536) port = p;
    }

    if (!host[0]) {
        print("folosire: ssh <gazda> [utilizator] [port]\n");
        return 1;
    }
    if (!user[0]) {
        print("Utilizator: ");
        readline(user, sizeof(user));
    }

    print("Rezolv "); print(host); print(" ...\n");
    uint32_t ip = host_resolve(host);
    if (!ip) { print("Nu pot rezolva gazda.\n"); return 1; }

    char pass[64];
    print("NOTA: DevOS foloseste layout US; daca vezi alte caractere decat parola\n");
    print("ta, tastatura nu se potriveste -> foloseste 'sshkey gen' + cheie.\n");
    print("Parola pentru "); print(user); print("@"); print(host);
    print(" (vizibila; Enter daca folosesti cheia): ");
    read_line_echo(pass, sizeof(pass), 1);

    print("Conectare la "); print_ip(ip); print_char(':'); print_num(port);
    print(" ...\n");

    if (ssh_open(ip, (uint16_t)port, user, pass) < 0) {
        print("Nu pot porni sesiunea SSH.\n");
        return 1;
    }

    int s;
    while ((s = ssh_status()) == 1) sleep_ms(40);
    if (s != 2) {
        char err[256];
        if (ssh_error(err, sizeof(err)) > 0 && err[0]) {
            print("Esec: "); print(err); print("\n");
        } else {
            print("Handshake/autentificare esuata.\n");
        }
        return 1;
    }
    print("Conectat! Shell securizat. Apasa Ctrl+Q pentru a inchide.\n\n");

    uint8_t rbuf[1024];
    for (;;) {
        int got = 0;
        int n = ssh_read(rbuf, sizeof(rbuf));
        if (n > 0) {
            for (int k = 0; k < n; k++) print_char((char)rbuf[k]);
            got = 1;
        }
        int c = readc();
        if (c == 0x11) {                     /* Ctrl+Q */
            ssh_close();
            print("\n[inchis]\n");
            return 0;
        }
        if (c >= 0) {
            /* tastele speciale devin secvente ANSI (pt. mc, nano, editor etc.) */
            const char *seq = 0;
            /* sagetile: in mod aplicatie (DECCKM, setat de mc/ncurses) se trimit
             * \eO.. , altfel \e[.. — altfel programele ncurses nu le recunosc */
            int app = term_appcursor();
            switch (c) {
            case 0x80: case 0x90: seq = app ? "\x1bOA" : "\x1b[A"; break;  /* sus */
            case 0x81: case 0x91: seq = app ? "\x1bOB" : "\x1b[B"; break;  /* jos */
            case 0x82: case 0x92: seq = app ? "\x1bOD" : "\x1b[D"; break;  /* stanga */
            case 0x83: case 0x93: seq = app ? "\x1bOC" : "\x1b[C"; break;  /* dreapta */
            case 0x84:            seq = "\x1b[3~"; break;  /* Delete */
            case 0x85: case 0x94: seq = "\x1b[H"; break;   /* Home */
            case 0x86: case 0x95: seq = "\x1b[F"; break;   /* End */
            /* F1..F10 (coduri abstracte 0xB0..0xB9) -> secvente xterm */
            case 0xB0: seq = "\x1bOP"; break;        /* F1 */
            case 0xB1: seq = "\x1bOQ"; break;        /* F2 */
            case 0xB2: seq = "\x1bOR"; break;        /* F3 */
            case 0xB3: seq = "\x1bOS"; break;        /* F4 */
            case 0xB4: seq = "\x1b[15~"; break;      /* F5 */
            case 0xB5: seq = "\x1b[17~"; break;      /* F6 */
            case 0xB6: seq = "\x1b[18~"; break;      /* F7 */
            case 0xB7: seq = "\x1b[19~"; break;      /* F8 */
            case 0xB8: seq = "\x1b[20~"; break;      /* F9 */
            case 0xB9: seq = "\x1b[21~"; break;      /* F10 */
            }
            if (seq) {
                int m = 0; while (seq[m]) m++;
                ssh_write(seq, m);
            } else {
                char ch;
                if (c == '\n') ch = '\r';    /* Enter -> CR (ca un terminal) */
                else if (c == '\b') ch = (char)0x7F;
                else ch = (char)c;
                ssh_write(&ch, 1);
            }
            got = 1;
        }
        if (ssh_status() != 2) {
            /* mai golim ce a ramas */
            n = ssh_read(rbuf, sizeof(rbuf));
            for (int k = 0; k < n; k++) print_char((char)rbuf[k]);
            print("\n[conexiune inchisa]\n");
            return 0;
        }
        if (!got) sleep_ms(12);
    }
}
