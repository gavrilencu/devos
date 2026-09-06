#pragma once
#include <stdint.h>

/* Mini-biblioteca pentru programele user MyOS (ring 3).
 * Totul trece prin syscalls (int 0x80) — vezi kernel/syscall.h.
 * Un program defineste `int umain(const char *args)` — args e stringul
 * de dupa numele programului in comanda `run`. */

int umain(const char *args);

void print(const char *s);
void print_char(char c);
void print_num(int64_t v);
void write_buf(const void *p, uint64_t len);   /* scrie exact len bytes */

void uexit(void) __attribute__((noreturn));
int64_t getpid(void);
void sleep_ms(uint64_t ms);
uint64_t ticks(void);

/* codurile tastelor speciale livrate de kernel prin readc */
#define KEY_UP    0x80
#define KEY_DOWN  0x81
#define KEY_LEFT  0x82
#define KEY_RIGHT 0x83

int readc(void);                     /* -1 = nimic inca, -2 = EOF (pipe) */
char getc_blocking(void);            /* asteapta politicos (polling + sleep) */
void readline(char *buf, int max);   /* citeste o linie cu echo si backspace */
int read_char(void);                 /* blocant; -1 = EOF — pentru filtre */

/* Fisiere MyFS. fread_file intoarce numarul de bytes cititi sau -1;
 * fwrite_file intoarce 0 la succes. */
int64_t fread_file(const char *name, void *buf, uint64_t maxlen);
int64_t fwrite_file(const char *name, const void *buf, uint64_t len);
int64_t fdelete(const char *name);              /* 0 la succes */
int64_t flist(char *buf, uint64_t maxlen);      /* "nume marime\n" per fisier */

/* Procese. */
int64_t spawn(const char *name, const char *args);  /* id-ul sau -1 */
int alive(int64_t id);                              /* task-ul mai exista? */

/* Pipe-uri: pipe_create() da un id; spawn2 porneste un program cu
 * stdin/stdout legate la pipe-uri (-1 = consola/tastatura). */
int64_t pipe_create(void);
int64_t spawn2(const char *name, const char *args,
               int64_t in_pipe, int64_t out_pipe);

void clear_screen(void);
uint64_t rtc_time(void);   /* ora hardware: (hh<<16)|(mm<<8)|ss */

/* Retea. ip = adresa in ordinea gazdei (a<<24|b<<16|c<<8|d).
 * net_ping trimite un echo; net_ping_result: -1 astept, -2 timeout, altfel ms. */
void net_ping(uint32_t ip);
int  net_ping_result(void);

/* Client TCP (conexiune iesita). Toate sunt non-blocante. */
int  tcp_connect(uint32_t ip, uint16_t port);    /* handle 0..N sau -1 */
int  tcp_status(int h);        /* 0=inchis, 1=conectare, 2=stabilit */
int  tcp_send(int h, const void *buf, int len);  /* octeti trimisi sau -1 */
int  tcp_recv(int h, void *buf, int max);        /* octeti cititi (0=nimic) */
void tcp_close(int h);

/* DNS. dns_start porneste o cerere; dns_poll: -1 astept, 0 esec, altfel IP.
 * dns_resolve face polling pina la un rezultat (blocant, dar politicos). */
void dns_start(const char *name);
int64_t dns_poll(void);
uint32_t dns_resolve(const char *name);   /* 0 = esec */

uint32_t ip_parse(const char *s);         /* "a.b.c.d" -> IP, 0 = invalid */
uint32_t host_resolve(const char *s);     /* IP direct sau prin DNS; 0 = esec */

int64_t pslist(char *buf, uint64_t maxlen);   /* tabela de task-uri, ca text */
uint64_t meminfo(void);                       /* memoria fizica libera (bytes) */

/* Client SSH: deschide sesiune, apoi read/write date de canal (shell). */
int  ssh_open(uint32_t ip, uint16_t port, const char *user, const char *pass);
int  ssh_status(void);   /* 0=inchis, 1=in curs, 2=gata, -1=eroare */
int  ssh_read(void *buf, int max);
int  ssh_write(const void *buf, int len);
void ssh_close(void);
int ssh_error(char *buf, int max);
int ssh_keygen(void);
int ssh_pubkey(char *buf, int max);
int term_appcursor(void);        /* 1 daca terminalul e in mod DECCKM (sageti \eO) */
