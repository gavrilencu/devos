/* basic — interpretor BASIC clasic (linii numerotate) pentru MyOS.
 * Ruleaza in ring 3, citeste programul de pe MyFS.
 *
 * Utilizare: basic <fisier.bas>
 *
 * Limbajul (cuvintele-cheie sunt case-insensitive):
 *   10 rem comentariu
 *   20 let a = 5            (let e optional: "a = 5")
 *   30 print "text"; a      (; concateneaza; ; la final = fara newline)
 *   40 input b              (citeste un intreg de la tastatura)
 *   50 if a > b then print "da"    (relatii: = <> < > <= >=)
 *   60 goto 40
 *   70 for i = 1 to 10 step 2 ... next
 *   75 gosub 200 ... return   (subrutine, max 8 nivele)
 *   80 end
 * Expresii: intregi, variabile a-z, + - * /, paranteze, rnd(n). */

#include <stdint.h>
#include "lib/ulib.h"

#define MAX_LINES 256
#define MAX_FORS  8

static char src[16384];
static struct {
    int num;
    const char *text;
} lines[MAX_LINES];
static int nlines;

static int64_t vars[26];
static struct {
    int var;
    int64_t limit, step;
    int ret_ip;
} fors[MAX_FORS];
static int fsp;

#define MAX_GOSUB 8
static int gosub_stack[MAX_GOSUB];
static int gsp;

static int next_ip;
static int running;
static const char *err_msg;
static uint64_t rng;

static void skipsp(const char **p)
{
    while (**p == ' ')
        (*p)++;
}

/* potriveste un cuvant-cheie (case-insensitive), fara sa "muste" din
 * identificatori mai lungi */
static int kw(const char **pp, const char *w)
{
    const char *p = *pp;
    while (*w) {
        char c = *p;
        if (c >= 'A' && c <= 'Z')
            c = (char)(c + 32);
        if (c != *w)
            return 0;
        p++;
        w++;
    }
    char c = *p;
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))
        return 0;
    *pp = p;
    return 1;
}

/* o variabila e o singura litera a-z */
static int var_idx(const char **pp)
{
    char c = **pp;
    if (c >= 'A' && c <= 'Z')
        c = (char)(c + 32);
    if (c < 'a' || c > 'z')
        return -1;
    char nxt = (*pp)[1];
    if ((nxt >= 'a' && nxt <= 'z') || (nxt >= 'A' && nxt <= 'Z'))
        return -1;
    (*pp)++;
    return c - 'a';
}

static int64_t parse_expr(const char **p);

static int64_t parse_factor(const char **p)
{
    skipsp(p);
    if (**p == '-') {
        (*p)++;
        return -parse_factor(p);
    }
    if (**p == '(') {
        (*p)++;
        int64_t v = parse_expr(p);
        skipsp(p);
        if (**p == ')')
            (*p)++;
        else
            err_msg = "lipseste )";
        return v;
    }
    if (**p >= '0' && **p <= '9') {
        int64_t v = 0;
        while (**p >= '0' && **p <= '9')
            v = v * 10 + (*(*p)++ - '0');
        return v;
    }
    {
        const char *save = *p;
        if (kw(p, "rnd")) {
            skipsp(p);
            if (**p == '(') {
                (*p)++;
                int64_t n = parse_expr(p);
                skipsp(p);
                if (**p == ')')
                    (*p)++;
                else
                    err_msg = "lipseste )";
                if (n <= 0) {
                    err_msg = "rnd cere un numar pozitiv";
                    return 0;
                }
                rng = rng * 6364136223846793005ull + 1442695040888963407ull;
                return (int64_t)((rng >> 33) % (uint64_t)n);
            }
            *p = save;
        }
    }
    int vi = var_idx(p);
    if (vi >= 0)
        return vars[vi];
    err_msg = "expresie invalida";
    return 0;
}

static int64_t parse_term(const char **p)
{
    int64_t v = parse_factor(p);
    for (;;) {
        skipsp(p);
        if (**p == '*') {
            (*p)++;
            v *= parse_factor(p);
        } else if (**p == '/') {
            (*p)++;
            int64_t d = parse_factor(p);
            if (d == 0) {
                err_msg = "impartire la zero";
                return 0;
            }
            v /= d;
        } else {
            break;
        }
    }
    return v;
}

static int64_t parse_expr(const char **p)
{
    int64_t v = parse_term(p);
    for (;;) {
        skipsp(p);
        if (**p == '+') {
            (*p)++;
            v += parse_term(p);
        } else if (**p == '-') {
            (*p)++;
            v -= parse_term(p);
        } else {
            break;
        }
    }
    return v;
}

static int parse_cond(const char **p)
{
    int64_t a = parse_expr(p);
    skipsp(p);
    int op;
    if (**p == '=') {
        op = 1;
        (*p)++;
    } else if (**p == '<') {
        (*p)++;
        if (**p == '>') { op = 2; (*p)++; }
        else if (**p == '=') { op = 5; (*p)++; }
        else op = 3;
    } else if (**p == '>') {
        (*p)++;
        if (**p == '=') { op = 6; (*p)++; }
        else op = 4;
    } else {
        err_msg = "conditie invalida";
        return 0;
    }
    int64_t b = parse_expr(p);
    switch (op) {
    case 1: return a == b;
    case 2: return a != b;
    case 3: return a < b;
    case 4: return a > b;
    case 5: return a <= b;
    default: return a >= b;
    }
}

static int find_line(int num)
{
    for (int i = 0; i < nlines; i++)
        if (lines[i].num == num)
            return i;
    return -1;
}

static void exec_stmt(const char *p)
{
    skipsp(&p);
    if (*p == '\0')
        return;
    if (kw(&p, "rem"))
        return;
    if (kw(&p, "end")) {
        running = 0;
        return;
    }
    if (kw(&p, "goto")) {
        int64_t n = parse_expr(&p);
        if (err_msg)
            return;
        int t = find_line((int)n);
        if (t < 0) {
            err_msg = "linia din goto nu exista";
            return;
        }
        next_ip = t;
        return;
    }
    if (kw(&p, "gosub")) {
        int64_t n = parse_expr(&p);
        if (err_msg)
            return;
        int t = find_line((int)n);
        if (t < 0) {
            err_msg = "linia din gosub nu exista";
            return;
        }
        if (gsp >= MAX_GOSUB) {
            err_msg = "prea multe gosub-uri imbricate";
            return;
        }
        gosub_stack[gsp++] = next_ip;   /* ne intoarcem la linia urmatoare */
        next_ip = t;
        return;
    }
    if (kw(&p, "return")) {
        if (gsp == 0) {
            err_msg = "return fara gosub";
            return;
        }
        next_ip = gosub_stack[--gsp];
        return;
    }
    if (kw(&p, "print")) {
        int nl = 1;
        for (;;) {
            skipsp(&p);
            if (*p == '\0' || err_msg)
                break;
            if (*p == '"') {
                p++;
                while (*p && *p != '"')
                    print_char(*p++);
                if (*p == '"') {
                    p++;
                } else {
                    err_msg = "string neterminat";
                    break;
                }
            } else {
                int64_t v = parse_expr(&p);
                if (err_msg)
                    break;
                print_num(v);
            }
            skipsp(&p);
            if (*p == ';') {
                p++;
                skipsp(&p);
                if (*p == '\0') {
                    nl = 0;
                    break;
                }
            } else {
                break;
            }
        }
        if (!err_msg && nl)
            print("\n");
        return;
    }
    if (kw(&p, "input")) {
        skipsp(&p);
        int vi = var_idx(&p);
        if (vi < 0) {
            err_msg = "input cere o variabila";
            return;
        }
        char buf[32];
        print("? ");
        readline(buf, sizeof(buf));
        const char *q = buf;
        int neg = 0;
        if (*q == '-') {
            neg = 1;
            q++;
        }
        int64_t v = 0;
        while (*q >= '0' && *q <= '9')
            v = v * 10 + (*q++ - '0');
        vars[vi] = neg ? -v : v;
        return;
    }
    if (kw(&p, "if")) {
        int c = parse_cond(&p);
        if (err_msg)
            return;
        skipsp(&p);
        if (!kw(&p, "then")) {
            err_msg = "lipseste then";
            return;
        }
        if (c)
            exec_stmt(p);
        return;
    }
    if (kw(&p, "for")) {
        skipsp(&p);
        int vi = var_idx(&p);
        skipsp(&p);
        if (vi < 0 || *p != '=') {
            err_msg = "for invalid (astept: for i = a to b)";
            return;
        }
        p++;
        int64_t start = parse_expr(&p);
        skipsp(&p);
        if (!kw(&p, "to")) {
            err_msg = "lipseste to";
            return;
        }
        int64_t limit = parse_expr(&p);
        int64_t step = 1;
        skipsp(&p);
        if (kw(&p, "step"))
            step = parse_expr(&p);
        if (err_msg)
            return;
        if (fsp >= MAX_FORS) {
            err_msg = "prea multe for-uri imbricate";
            return;
        }
        vars[vi] = start;
        fors[fsp].var = vi;
        fors[fsp].limit = limit;
        fors[fsp].step = step;
        fors[fsp].ret_ip = next_ip;   /* prima linie a corpului */
        fsp++;
        return;
    }
    if (kw(&p, "next")) {
        if (fsp == 0) {
            err_msg = "next fara for";
            return;
        }
        int vi = fors[fsp - 1].var;
        vars[vi] += fors[fsp - 1].step;
        int cont = fors[fsp - 1].step >= 0
                       ? (vars[vi] <= fors[fsp - 1].limit)
                       : (vars[vi] >= fors[fsp - 1].limit);
        if (cont)
            next_ip = fors[fsp - 1].ret_ip;
        else
            fsp--;
        return;
    }
    if (kw(&p, "let"))
        skipsp(&p);

    /* atribuire: var = expr */
    {
        const char *save = p;
        int vi = var_idx(&p);
        skipsp(&p);
        if (vi >= 0 && *p == '=') {
            p++;
            int64_t v = parse_expr(&p);
            if (!err_msg)
                vars[vi] = v;
            return;
        }
        p = save;
    }
    err_msg = "instructiune necunoscuta";
}

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
        print("utilizare: basic <fisier.bas>\n");
        return 1;
    }

    int64_t n = fread_file(fname, src, sizeof(src) - 1);
    if (n < 0) {
        print("nu exista '");
        print(fname);
        print("'\n");
        return 1;
    }
    src[n] = '\0';

    /* impartim sursa in linii numerotate */
    char *s = src;
    while (*s && nlines < MAX_LINES) {
        char *start = s;
        while (*s && *s != '\n')
            s++;
        if (*s)
            *s++ = '\0';

        const char *q = start;
        while (*q == ' ')
            q++;
        if (*q == '\0')
            continue;
        if (*q < '0' || *q > '9') {
            print("linie fara numar: ");
            print(q);
            print("\n");
            return 1;
        }
        int num = 0;
        while (*q >= '0' && *q <= '9')
            num = num * 10 + (*q++ - '0');
        while (*q == ' ')
            q++;
        lines[nlines].num = num;
        lines[nlines].text = q;
        nlines++;
    }

    rng = ticks() * 2654435761ull + 88172645463325252ull;

    int ip = 0;
    running = 1;
    while (running && ip < nlines) {
        next_ip = ip + 1;
        exec_stmt(lines[ip].text);
        if (err_msg) {
            print("EROARE la linia ");
            print_num(lines[ip].num);
            print(": ");
            print(err_msg);
            print("\n");
            return 1;
        }
        ip = next_ip;
    }
    return 0;
}
