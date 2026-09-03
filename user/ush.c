/* ush — shell-ul user al lui MyOS. Ruleaza in ring 3 si vorbeste cu
 * kernelul exclusiv prin syscalls: exact ca un shell adevarat.
 *
 * Builtin-uri: help, echo, ls, cat, rm, uptime, clear, exit.
 * Orice alta comanda e cautata pe disc si pornita ca proces nou
 * (ex: "calc", "edit nota.txt", "guess"); un "&" la final = fundal. */

#include <stdint.h>
#include "lib/ulib.h"

static char line[128];
static char fbuf[8192];

static int streq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/* desparte "cmd args..." in numele comenzii si argumente (in-place) */
static void split(char *s, char **cmd, char **args)
{
    while (*s == ' ')
        s++;
    *cmd = s;
    while (*s && *s != ' ')
        s++;
    if (*s) {
        *s++ = '\0';
        while (*s == ' ')
            s++;
    }
    *args = s;
    int n = 0;
    while ((*args)[n])
        n++;
    while (n > 0 && (*args)[n - 1] == ' ')
        (*args)[--n] = '\0';
}

/* pipeline cu doua etaje: stdout-ul primului -> stdin-ul celui de-al doilea */
static void run_pipeline(char *left, char *right)
{
    char *lc, *la, *rc, *ra;
    split(left, &lc, &la);
    split(right, &rc, &ra);
    if (*lc == '\0' || *rc == '\0') {
        print("utilizare: prog1 [args] | prog2 [args]\n");
        return;
    }

    int64_t p = pipe_create();
    if (p < 0) {
        print("nu mai sunt pipe-uri libere\n");
        return;
    }

    int64_t a = spawn2(lc, la, -1, p);
    int64_t b = spawn2(rc, ra, p, -1);
    if (a < 0) {
        print("nu exista '");
        print(lc);
        print("'\n");
    }
    if (b < 0) {
        print("nu exista '");
        print(rc);
        print("'\n");
    }

    while ((a >= 0 && alive(a)) || (b >= 0 && alive(b)))
        sleep_ms(30);
}

static void run_program(const char *name, const char *args, int bg)
{
    int64_t id = spawn(name, args);
    if (id < 0) {
        print("comanda necunoscuta sau program lipsa: '");
        print(name);
        print("' (scrie 'help')\n");
        return;
    }
    if (bg) {
        print("pornit in fundal (task ");
        print_num(id);
        print(")\n");
        return;
    }
    /* foreground: asteptam fara sa citim tastatura — e a copilului */
    while (alive(id))
        sleep_ms(30);
}

int umain(const char *args)
{
    (void)args;
    print("ush - shell-ul user MyOS (ring 3). Scrie 'help'.\n");

    for (;;) {
        print("ush> ");
        readline(line, sizeof(line));

        /* "a | b" = pipeline intre doua programe */
        char *bar = 0;
        for (char *p = line; *p; p++)
            if (*p == '|') {
                bar = p;
                break;
            }
        if (bar) {
            *bar = '\0';
            run_pipeline(line, bar + 1);
            continue;
        }

        char *cmd = line;
        while (*cmd == ' ')
            cmd++;
        if (*cmd == '\0')
            continue;

        char *rest = cmd;
        while (*rest && *rest != ' ')
            rest++;
        if (*rest) {
            *rest++ = '\0';
            while (*rest == ' ')
                rest++;
        }

        /* "&" oriunde in argumente = ruleaza in fundal */
        int bg = 0;
        for (char *p = rest; *p; p++)
            if (*p == '&') {
                *p = ' ';
                bg = 1;
            }
        int rl = 0;
        while (rest[rl])
            rl++;
        while (rl > 0 && rest[rl - 1] == ' ')
            rest[--rl] = '\0';

        if (streq(cmd, "help")) {
            print("builtin: help, echo, ls, cat, cp, mv, rm, ps, mem, date, ping, uptime, clear, exit\n");
            print("retea: nslookup <nume>, telnet <gazda> [port], fetch <gazda> [cale]\n");
            print("orice altceva = program de pe disc: calc, guess, edit <f>, basic <f>\n");
            print("pipeline: prog1 | prog2; '&' = fundal; Alt+F1..F3 = alt terminal\n");
            print("sagetile sus/jos = istoricul comenzilor\n");
        } else if (streq(cmd, "ps")) {
            int64_t n = pslist(fbuf, sizeof(fbuf) - 1);
            if (n > 0) {
                fbuf[n] = '\0';
                print(fbuf);
            }
        } else if (streq(cmd, "mem")) {
            print_num((int64_t)(meminfo() / 1024));
            print(" KiB de memorie fizica libera\n");
        } else if (streq(cmd, "echo")) {
            print(rest);
            print("\n");
        } else if (streq(cmd, "ls")) {
            int64_t n = flist(fbuf, sizeof(fbuf) - 1);
            if (n > 0) {
                fbuf[n] = '\0';
                print(fbuf);
            } else {
                print("(niciun fisier)\n");
            }
        } else if (streq(cmd, "cat")) {
            if (*rest == '\0') {
                print("utilizare: cat <fisier>\n");
                continue;
            }
            int64_t n = fread_file(rest, fbuf, sizeof(fbuf) - 1);
            if (n < 0) {
                print("nu exista '");
                print(rest);
                print("'\n");
                continue;
            }
            fbuf[n] = '\0';
            print(fbuf);
            if (n > 0 && fbuf[n - 1] != '\n')
                print("\n");
        } else if (streq(cmd, "rm")) {
            if (*rest == '\0') {
                print("utilizare: rm <fisier>\n");
                continue;
            }
            int64_t rr = fdelete(rest);
            if (rr == 0)
                print("sters\n");
            else if (rr == -2)
                print("fisier de sistem, protejat\n");
            else
                print("nu exista\n");
        } else if (streq(cmd, "cp") || streq(cmd, "mv")) {
            char *a = rest, *b2 = rest;
            while (*b2 && *b2 != ' ')
                b2++;
            if (*b2) {
                *b2++ = '\0';
                while (*b2 == ' ')
                    b2++;
            }
            if (*a == '\0' || *b2 == '\0') {
                print("utilizare: cp/mv <sursa> <destinatie>\n");
                continue;
            }
            int64_t nn = fread_file(a, fbuf, sizeof(fbuf));
            if (nn < 0) {
                print("nu exista '");
                print(a);
                print("'\n");
            } else if (nn == (int64_t)sizeof(fbuf)) {
                print("fisier prea mare pentru cp/mv (max 8 KiB)\n");
            } else if (fwrite_file(b2, fbuf, (uint64_t)nn) != 0) {
                print("eroare la scriere\n");
            } else {
                if (cmd[0] == 'm')
                    fdelete(a);
                print(cmd[0] == 'm' ? "mutat\n" : "copiat\n");
            }
        } else if (streq(cmd, "ping")) {
            /* parseaza IP in forma a.b.c.d */
            uint32_t ip = 0;
            int oct = 0, val = 0, seen = 0, ok = 1;
            const char *q = rest;
            for (;; q++) {
                if (*q >= '0' && *q <= '9') {
                    val = val * 10 + (*q - '0');
                    seen = 1;
                } else if (*q == '.' || *q == '\0') {
                    if (!seen || val > 255) { ok = 0; break; }
                    ip = (ip << 8) | (uint32_t)val;
                    oct++;
                    val = 0;
                    seen = 0;
                    if (*q == '\0') break;
                } else { ok = 0; break; }
            }
            if (!ok || oct != 4) {
                print("utilizare: ping <a.b.c.d>  (ex: ping 10.0.2.2)\n");
                continue;
            }
            print("ping ");
            print(rest);
            print(" ...\n");
            for (int t = 0; t < 4; t++) {
                net_ping(ip);
                int r;
                while ((r = net_ping_result()) == -1)
                    sleep_ms(20);
                if (r == -2) {
                    print("  timeout\n");
                } else {
                    print("  raspuns in ");
                    print_num(r);
                    print(" ms\n");
                }
                sleep_ms(300);
            }
        } else if (streq(cmd, "ssh")) {
            print("ssh necesita criptografie (schimb de chei, AES) - in lucru.\n");
            print("fundatia de retea (DNS, TCP client, HTTP) e gata; cripto urmeaza.\n");
        } else if (streq(cmd, "date")) {
            uint64_t t = rtc_time();
            char s[9];
            s[0] = (char)('0' + ((t >> 16) & 0xFF) / 10);
            s[1] = (char)('0' + ((t >> 16) & 0xFF) % 10);
            s[2] = ':';
            s[3] = (char)('0' + ((t >> 8) & 0xFF) / 10);
            s[4] = (char)('0' + ((t >> 8) & 0xFF) % 10);
            s[5] = ':';
            s[6] = (char)('0' + (t & 0xFF) / 10);
            s[7] = (char)('0' + (t & 0xFF) % 10);
            s[8] = '\0';
            print(s);
            print("\n");
        } else if (streq(cmd, "uptime")) {
            uint64_t s = ticks() / 100;
            print_num((int64_t)(s / 60));
            print("m ");
            print_num((int64_t)(s % 60));
            print("s\n");
        } else if (streq(cmd, "clear")) {
            clear_screen();
        } else if (streq(cmd, "exit")) {
            print("ush se inchide (init va porni unul nou pe acest terminal)\n");
            break;
        } else {
            run_program(cmd, rest, bg);
        }
    }
    return 0;
}
