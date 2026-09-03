/* edit — editor de text pe linii pentru MyOS (program C, ring 3).
 *
 * Utilizare: edit <fisier>   (sau fara argument: intreaba numele)
 * Comenzi:
 *   l           listeaza tot, cu numere de linie
 *   p N [M]     listeaza linia N sau intervalul N..M
 *   a           adauga linii la final (o linie cu doar "." termina)
 *   i N         insereaza linii INAINTE de linia N (pana la ".")
 *   r N         inlocuieste linia N (citeste o singura linie)
 *   d N         sterge linia N
 *   f text      cauta textul si listeaza liniile care il contin
 *   n           statistici (linii, bytes)
 *   w           salveaza pe disc
 *   q           iesire
 */

#include <stdint.h>
#include "lib/ulib.h"

static char fname[24];
static char text[16384 + 1];
static int tlen;
static char line[128];

static int count_lines(void)
{
    int n = 0;
    for (int i = 0; i < tlen; i++)
        if (text[i] == '\n')
            n++;
    return n;
}

/* gaseste offseturile liniei `ln` (1-based); e = dupa \n-ul ei */
static int line_bounds(int ln, int *s, int *e)
{
    int cur = 1, i = 0;
    while (cur < ln) {
        while (i < tlen && text[i] != '\n')
            i++;
        if (i >= tlen)
            return -1;
        i++;
        cur++;
    }
    if (i >= tlen)
        return -1;
    *s = i;
    while (i < tlen && text[i] != '\n')
        i++;
    *e = (i < tlen) ? i + 1 : i;
    return 0;
}

static void print_one(int ln, int s, int e)
{
    print_num(ln);
    print(": ");
    int end = e;
    if (end > s && text[end - 1] == '\n')
        end--;
    char saved = text[end];
    text[end] = '\0';
    print(&text[s]);
    text[end] = saved;
    print("\n");
}

static void do_list(int from, int to)
{
    if (tlen == 0) {
        print("(fisier gol)\n");
        return;
    }
    int ln = 1, i = 0;
    while (i < tlen) {
        int j = i;
        while (j < tlen && text[j] != '\n')
            j++;
        int e = (j < tlen) ? j + 1 : j;
        if (ln >= from && ln <= to)
            print_one(ln, i, e);
        i = e;
        ln++;
    }
}

/* insereaza `len` bytes la offsetul `at`; 0 la succes */
static int insert_at(int at, const char *s, int len)
{
    if (tlen + len > (int)sizeof(text) - 1) {
        print("bufferul e plin!\n");
        return -1;
    }
    for (int k = tlen - 1; k >= at; k--)
        text[k + len] = text[k];
    for (int k = 0; k < len; k++)
        text[at + k] = s[k];
    tlen += len;
    return 0;
}

static void delete_range(int s, int e)
{
    for (int k = 0; k < tlen - e; k++)
        text[s + k] = text[e + k];
    tlen -= e - s;
}

/* citeste linii pana la "." si le insereaza incepand de la offsetul `at` */
static void read_lines_into(int at)
{
    print("scrie linii; o linie cu doar \".\" termina\n");
    for (;;) {
        readline(line, sizeof(line));
        if (line[0] == '.' && line[1] == '\0')
            break;
        int ll = 0;
        while (line[ll])
            ll++;
        line[ll] = '\n';
        if (insert_at(at, line, ll + 1) < 0)
            break;
        at += ll + 1;
    }
}

static void do_find(const char *needle)
{
    int nl = 0;
    while (needle[nl])
        nl++;
    if (nl == 0) {
        print("utilizare: f <text>\n");
        return;
    }
    int ln = 1, i = 0, found = 0;
    while (i < tlen) {
        int j = i;
        while (j < tlen && text[j] != '\n')
            j++;
        for (int k = i; k + nl <= j; k++) {
            int m = 0;
            while (m < nl && text[k + m] == needle[m])
                m++;
            if (m == nl) {
                print_one(ln, i, (j < tlen) ? j + 1 : j);
                found++;
                break;
            }
        }
        i = (j < tlen) ? j + 1 : j;
        ln++;
    }
    if (!found)
        print("negasit\n");
}

static int parse_int(const char **p)
{
    while (**p == ' ')
        (*p)++;
    if (**p < '0' || **p > '9')
        return -1;
    int v = 0;
    while (**p >= '0' && **p <= '9')
        v = v * 10 + (*(*p)++ - '0');
    return v;
}

int umain(const char *args)
{
    int i = 0;
    while (args && args[i] && args[i] != ' ' && i < 23) {
        fname[i] = args[i];
        i++;
    }
    fname[i] = '\0';
    if (fname[0] == '\0') {
        print("fisier: ");
        readline(fname, sizeof(fname));
        if (fname[0] == '\0') {
            print("renunt.\n");
            return 0;
        }
    }

    int64_t n = fread_file(fname, text, sizeof(text) - 1);
    if (n >= 0) {
        tlen = (int)n;
        print("incarcat '");
        print(fname);
        print("' (");
        print_num(n);
        print(" bytes, ");
        print_num(count_lines());
        print(" linii)\n");
    } else {
        tlen = 0;
        print("fisier nou: '");
        print(fname);
        print("'\n");
    }
    print("comenzi: l, p N [M], a, i N, r N, d N, f text, n, w, q\n");

    for (;;) {
        print("edit> ");
        readline(line, sizeof(line));
        const char *p = line + 1;
        int s, e, ln;

        switch (line[0]) {
        case '\0':
            break;
        case 'q':
            return 0;
        case 'l':
            do_list(1, 1 << 30);
            break;
        case 'p': {
            int from = parse_int(&p);
            if (from < 0) {
                do_list(1, 1 << 30);
                break;
            }
            int to = parse_int(&p);
            do_list(from, to < 0 ? from : to);
            break;
        }
        case 'a':
            read_lines_into(tlen);
            break;
        case 'i':
            ln = parse_int(&p);
            if (ln < 1 || line_bounds(ln, &s, &e) < 0) {
                print("utilizare: i N (linia trebuie sa existe)\n");
                break;
            }
            read_lines_into(s);
            break;
        case 'r':
            ln = parse_int(&p);
            if (ln < 1 || line_bounds(ln, &s, &e) < 0) {
                print("utilizare: r N\n");
                break;
            }
            print("noua linie ");
            print_num(ln);
            print(": ");
            readline(line, sizeof(line));
            {
                int ll = 0;
                while (line[ll])
                    ll++;
                line[ll] = '\n';
                delete_range(s, e);
                insert_at(s, line, ll + 1);
            }
            break;
        case 'd':
            ln = parse_int(&p);
            if (ln < 1 || line_bounds(ln, &s, &e) < 0) {
                print("utilizare: d N\n");
                break;
            }
            delete_range(s, e);
            print("linie stearsa\n");
            break;
        case 'f':
            while (*p == ' ')
                p++;
            do_find(p);
            break;
        case 'n':
            print_num(count_lines());
            print(" linii, ");
            print_num(tlen);
            print(" bytes (capacitate ");
            print_num((int)sizeof(text) - 1);
            print(")\n");
            break;
        case 'w':
            if (tlen == 0) {
                print("nimic de salvat (fisierul ar fi gol)\n");
            } else if (fwrite_file(fname, text, (uint64_t)tlen) == 0) {
                print("salvat '");
                print(fname);
                print("' (");
                print_num(tlen);
                print(" bytes)\n");
            } else {
                print("eroare la salvare\n");
            }
            break;
        default:
            print("comenzi: l, p N [M], a, i N, r N, d N, f text, n, w, q\n");
        }
    }
}
