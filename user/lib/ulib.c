#include <stdint.h>
#include "ulib.h"

/* Conventia de syscall MyOS: RAX = numarul, RDI/RSI = argumente,
 * rezultatul vine in RAX. Kernelul pastreaza toate celelalte registre. */
static inline int64_t syscall2(int64_t n, int64_t a, int64_t b)
{
    int64_t ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(n), "D"(a), "S"(b)
                     : "memory");
    return ret;
}

static inline int64_t syscall3(int64_t n, int64_t a, int64_t b, int64_t c)
{
    int64_t ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(n), "D"(a), "S"(b), "d"(c)
                     : "memory");
    return ret;
}

static uint64_t ustrlen(const char *s)
{
    uint64_t n = 0;
    while (*s++)
        n++;
    return n;
}

/* Scrie tot bufferul, cu reincercare: spre un pipe plin, write-ul e
 * partial (sau 0) si asteptam cititorul; -2 = cititorul a murit. */
void write_buf(const void *p, uint64_t len)
{
    const char *s = p;
    while (len) {
        uint64_t chunk = len > 2048 ? 2048 : len;
        int64_t n = syscall2(0, (int64_t)s, (int64_t)chunk);
        if (n < 0)
            return;
        if (n == 0) {
            sleep_ms(5);
            continue;
        }
        s += n;
        len -= (uint64_t)n;
    }
}

void print(const char *s)
{
    write_buf(s, ustrlen(s));
}

void print_char(char c)
{
    write_buf(&c, 1);
}

void print_num(int64_t v)
{
    char buf[24];
    char *p = buf + sizeof(buf) - 1;
    *p = '\0';

    int neg = v < 0;
    uint64_t u = neg ? (uint64_t)-v : (uint64_t)v;
    do {
        *--p = (char)('0' + u % 10);
        u /= 10;
    } while (u);
    if (neg)
        *--p = '-';
    print(p);
}

void uexit(void)
{
    syscall2(1, 0, 0);
    for (;;)
        ;
}

int64_t getpid(void)
{
    return syscall2(2, 0, 0);
}

void sleep_ms(uint64_t ms)
{
    syscall2(3, (int64_t)ms, 0);
}

int readc(void)
{
    return (int)syscall2(4, 0, 0);
}

uint64_t ticks(void)
{
    return (uint64_t)syscall2(5, 0, 0);
}

char getc_blocking(void)
{
    int c;
    while ((c = readc()) == -1)
        sleep_ms(15);
    if (c == -2)
        return '\n';   /* EOF pe pipe: pentru cititorii de linii, ca un Enter */
    return (char)c;
}

int read_char(void)
{
    for (;;) {
        int c = readc();
        if (c == -2)
            return -1;    /* EOF */
        if (c >= 0)
            return c;
        sleep_ms(10);
    }
}

/* istoric de linii (sagetile sus/jos), pastrat per program */
#define HIST_N   8
#define HIST_LEN 128
static char hist[HIST_N][HIST_LEN];
static int hist_count;

static int str_eq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

void readline(char *buf, int max)
{
    int n = 0;
    int nav = -1;                  /* -1 = linia curenta (needitata din istoric) */

    for (;;) {
        int c;
        while ((c = readc()) == -1)
            sleep_ms(15);
        if (c == -2)
            c = '\n';              /* EOF pe pipe = Enter */

        if (c == KEY_UP || c == KEY_DOWN) {
            if (hist_count == 0)
                continue;
            int target;
            if (c == KEY_UP)
                target = (nav < 0) ? hist_count - 1 : (nav > 0 ? nav - 1 : 0);
            else {
                if (nav < 0)
                    continue;
                target = nav + 1;
            }
            while (n > 0) {        /* stergem ce e afisat */
                print_char('\b');
                n--;
            }
            if (target >= hist_count) {
                nav = -1;          /* sub istoric: linie goala */
                continue;
            }
            nav = target;
            for (const char *s = hist[nav]; *s && n < max - 1; s++) {
                buf[n++] = *s;
                print_char(*s);
            }
            continue;
        }
        if (c == KEY_LEFT || c == KEY_RIGHT)
            continue;

        if (c == '\n') {
            print_char('\n');
            break;
        }
        if (c == '\b') {
            if (n > 0) {
                n--;
                print_char('\b');
            }
            continue;
        }
        if (c < 32 || c > 126)
            continue;
        if (n < max - 1) {
            buf[n++] = (char)c;
            print_char((char)c);
        }
    }
    buf[n] = '\0';

    /* memoram in istoric (fara duplicate consecutive) */
    if (n > 0 && n < HIST_LEN &&
        (hist_count == 0 || !str_eq(hist[hist_count - 1], buf))) {
        if (hist_count == HIST_N) {
            for (int i = 0; i < HIST_N - 1; i++)
                for (int j = 0; j < HIST_LEN; j++)
                    hist[i][j] = hist[i + 1][j];
            hist_count--;
        }
        int i = 0;
        for (; buf[i]; i++)
            hist[hist_count][i] = buf[i];
        hist[hist_count][i] = '\0';
        hist_count++;
    }
}

int64_t fread_file(const char *name, void *buf, uint64_t maxlen)
{
    return syscall3(6, (int64_t)name, (int64_t)buf, (int64_t)maxlen);
}

int64_t fwrite_file(const char *name, const void *buf, uint64_t len)
{
    return syscall3(7, (int64_t)name, (int64_t)buf, (int64_t)len);
}

int64_t spawn(const char *name, const char *args)
{
    return syscall2(8, (int64_t)name, (int64_t)args);
}

int alive(int64_t id)
{
    return (int)syscall2(9, id, 0);
}

int64_t flist(char *buf, uint64_t maxlen)
{
    return syscall2(10, (int64_t)buf, (int64_t)maxlen);
}

int64_t fdelete(const char *name)
{
    return syscall2(11, (int64_t)name, 0);
}

void clear_screen(void)
{
    syscall2(12, 0, 0);
}

uint64_t rtc_time(void)
{
    return (uint64_t)syscall2(17, 0, 0);
}

void net_ping(uint32_t ip)
{
    syscall2(18, (int64_t)ip, 0);
}

int net_ping_result(void)
{
    return (int)syscall2(19, 0, 0);
}

int tcp_connect(uint32_t ip, uint16_t port)
{
    return (int)syscall2(20, (int64_t)ip, (int64_t)port);
}

int tcp_status(int h)
{
    return (int)syscall2(21, (int64_t)h, 0);
}

int tcp_send(int h, const void *buf, int len)
{
    return (int)syscall3(22, (int64_t)h, (int64_t)buf, (int64_t)len);
}

int tcp_recv(int h, void *buf, int max)
{
    return (int)syscall3(23, (int64_t)h, (int64_t)buf, (int64_t)max);
}

void tcp_close(int h)
{
    syscall2(24, (int64_t)h, 0);
}

void dns_start(const char *name)
{
    syscall2(25, (int64_t)name, 0);
}

int64_t dns_poll(void)
{
    return syscall2(26, 0, 0);
}

uint32_t dns_resolve(const char *name)
{
    dns_start(name);
    for (;;) {
        int64_t r = dns_poll();
        if (r >= 0)
            return (uint32_t)r;       /* 0 = esec, altfel IP */
        sleep_ms(30);
    }
}

uint32_t ip_parse(const char *s)
{
    uint32_t ip = 0;
    for (int part = 0; part < 4; part++) {
        if (*s < '0' || *s > '9')
            return 0;
        int v = 0, n = 0;
        while (*s >= '0' && *s <= '9') {
            v = v * 10 + (*s - '0');
            s++;
            if (++n > 3 || v > 255)
                return 0;
        }
        ip = (ip << 8) | (uint32_t)v;
        if (part < 3) {
            if (*s != '.')
                return 0;
            s++;
        }
    }
    return *s == '\0' ? ip : 0;
}

uint32_t host_resolve(const char *s)
{
    uint32_t ip = ip_parse(s);
    if (ip)
        return ip;
    return dns_resolve(s);
}

static inline int64_t syscall4(int64_t n, int64_t a, int64_t b,
                               int64_t c, int64_t d)
{
    int64_t ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(n), "D"(a), "S"(b), "d"(c), "c"(d)
                     : "memory");
    return ret;
}

int64_t pipe_create(void)
{
    return syscall2(15, 0, 0);
}

int64_t spawn2(const char *name, const char *args,
               int64_t in_pipe, int64_t out_pipe)
{
    return syscall4(16, (int64_t)name, (int64_t)args, in_pipe, out_pipe);
}

int64_t pslist(char *buf, uint64_t maxlen)
{
    return syscall2(13, (int64_t)buf, (int64_t)maxlen);
}

uint64_t meminfo(void)
{
    return (uint64_t)syscall2(14, 0, 0);
}

/* Punctul de intrare: kernelul sare aici cu stiva pregatita si cu
 * argumentele programului (string) in RDI. Programele definesc umain()
 * — "main" ar avea semnatura speciala impusa de compilator. */
extern int umain(const char *args);

void _start(const char *args);
void _start(const char *args)
{
    umain(args);
    uexit();
}
