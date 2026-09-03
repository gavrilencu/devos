/* Calculator interactiv — program user MyOS scris in C, ruland in ring 3.
 * Compilat freestanding, linkat la 0x8000000000, incarcat de kernel prin
 * loaderul ELF. Ex: "12 * 34", "100 / 7", "5 - 9"; linie goala = iesire. */

#include <stdint.h>
#include "lib/ulib.h"

/* buffer static in .bss — demonstreaza ca loaderul ELF aloca si zona
 * memsz > filesz */
static char line[128];

static const char *skip_spaces(const char *p)
{
    while (*p == ' ')
        p++;
    return p;
}

/* parseaza un intreg cu semn; intoarce pointerul dupa numar,
 * sau 0 daca nu incepe cu un numar */
static const char *parse_num(const char *p, int64_t *out)
{
    int neg = 0;
    if (*p == '-') {
        neg = 1;
        p++;
    }
    if (*p < '0' || *p > '9')
        return 0;
    int64_t v = 0;
    while (*p >= '0' && *p <= '9')
        v = v * 10 + (*p++ - '0');
    *out = neg ? -v : v;
    return p;
}

int umain(const char *args)
{
    (void)args;
    print("Calculator MyOS - program C in ring 3 (pid ");
    print_num(getpid());
    print(")\n");
    print("Exemplu: 12 * 34   (operatori: + - * /; linie goala = iesire)\n");

    for (;;) {
        print("calc> ");
        readline(line, sizeof(line));
        if (line[0] == '\0')
            break;

        const char *p = skip_spaces(line);
        int64_t a, b;

        p = parse_num(p, &a);
        if (!p) {
            print("expresie invalida\n");
            continue;
        }
        p = skip_spaces(p);
        char op = *p;
        if (op != '+' && op != '-' && op != '*' && op != '/') {
            print("operator necunoscut (foloseste + - * /)\n");
            continue;
        }
        p = skip_spaces(p + 1);
        p = parse_num(p, &b);
        if (!p) {
            print("expresie invalida\n");
            continue;
        }

        int64_t r;
        if (op == '+') {
            r = a + b;
        } else if (op == '-') {
            r = a - b;
        } else if (op == '*') {
            r = a * b;
        } else {
            if (b == 0) {
                print("impartire la zero!\n");
                continue;
            }
            r = a / b;
        }

        print("= ");
        print_num(r);
        print("\n");
    }

    print("la revedere!\n");
    return 0;
}
