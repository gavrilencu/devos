/* TCP minimal: server telnet pe portul 23 + client outbound (active open).
 * Simplificari: in-ordine, fara retransmisii, o fereastra fixa. Suficient
 * pentru un shell de retea si pentru un client telnet/HTTP din MyOS. */

#include "netstack.h"
#include "fs.h"
#include "kheap.h"
#include "pit.h"
#include "string.h"
#include "kprintf.h"

static uint16_t bswap16(uint16_t v) { return (uint16_t)((v << 8) | (v >> 8)); }
static uint32_t bswap32(uint32_t v)
{
    return (v << 24) | ((v & 0xFF00) << 8) | ((v >> 8) & 0xFF00) | (v >> 24);
}
#define htons(x) bswap16(x)
#define ntohs(x) bswap16(x)
#define htonl(x) bswap32(x)
#define ntohl(x) bswap32(x)

#define F_FIN 0x01
#define F_SYN 0x02
#define F_RST 0x04
#define F_PSH 0x08
#define F_ACK 0x10

#define SRV_PORT 23

struct tcphdr {
    uint16_t sport, dport;
    uint32_t seq, ack;
    uint8_t off;
    uint8_t flags;
    uint16_t win;
    uint16_t csum;
    uint16_t urg;
} __attribute__((packed));

enum { CLOSED, SYN_SENT, SYN_RCVD, ESTABLISHED };

struct tcp_conn {
    int state;
    uint32_t rip;
    uint16_t rport, lport;
    uint32_t snd_nxt, rcv_nxt;
    /* buffer de receptie (doar la client) */
    uint8_t *rx;
    int rx_head, rx_tail, rx_cap;
    uint64_t t0;                 /* moment pentru timeout de conectare */
};

/* ---- checksum TCP cu pseudo-antet (src, dst, proto=6, lungime) ---- */
static uint16_t tcp_csum(uint32_t src, uint32_t dst,
                         const void *seg, uint16_t len)
{
    static uint8_t buf[1600];
    int p = 0;
    buf[p++] = (uint8_t)(src >> 24); buf[p++] = (uint8_t)(src >> 16);
    buf[p++] = (uint8_t)(src >> 8);  buf[p++] = (uint8_t)src;
    buf[p++] = (uint8_t)(dst >> 24); buf[p++] = (uint8_t)(dst >> 16);
    buf[p++] = (uint8_t)(dst >> 8);  buf[p++] = (uint8_t)dst;
    buf[p++] = 0;
    buf[p++] = 6;
    buf[p++] = (uint8_t)(len >> 8);
    buf[p++] = (uint8_t)len;
    memcpy(buf + p, seg, len);
    return net_checksum(buf, p + len);
}

static void tcp_out(struct tcp_conn *c, uint8_t flags,
                    const void *data, uint16_t dlen)
{
    static uint8_t seg[1600];
    struct tcphdr *t = (struct tcphdr *)seg;
    t->sport = htons(c->lport);
    t->dport = htons(c->rport);
    t->seq = htonl(c->snd_nxt);
    t->ack = htonl(c->rcv_nxt);
    t->off = 5 << 4;                 /* 20 bytes, fara optiuni */
    t->flags = flags;
    t->win = htons(8192);
    t->csum = 0;
    t->urg = 0;
    if (dlen)
        memcpy(seg + 20, data, dlen);
    uint16_t seglen = (uint16_t)(20 + dlen);
    t->csum = htons(tcp_csum(IP_ADDR, c->rip, seg, seglen));
    ip_send(c->rip, 6, seg, seglen);
}

/* =====================================================================
 *  SERVER telnet (portul 23) — un shell de retea, o conexiune
 * ===================================================================== */

static struct tcp_conn srv;
static char line[256];
static int line_len;

static void srv_write(const char *s, int len)
{
    if (srv.state != ESTABLISHED)
        return;
    tcp_out(&srv, F_PSH | F_ACK, s, (uint16_t)len);
    srv.snd_nxt += len;
}

static void srv_puts(const char *s)
{
    int n = 0;
    while (s[n])
        n++;
    srv_write(s, n);
}

static void net_shell(char *cmd)
{
    char *arg = cmd;
    while (*arg && *arg != ' ')
        arg++;
    if (*arg) {
        *arg++ = '\0';
        while (*arg == ' ')
            arg++;
    }

    if (strcmp(cmd, "help") == 0) {
        srv_puts("Comenzi: help, ver, ls, cat <f>, mem, uptime, echo <t>, exit\r\n");
    } else if (strcmp(cmd, "ver") == 0) {
        srv_puts("MyOS v0.35 - shell de retea (telnet)\r\n");
    } else if (strcmp(cmd, "echo") == 0) {
        srv_puts(arg);
        srv_puts("\r\n");
    } else if (strcmp(cmd, "uptime") == 0) {
        char b[32];
        uint64_t s = pit_ticks() / 100;
        int p = 0;
        uint64_t v = s;
        char t[24];
        int i = 0;
        do { t[i++] = (char)('0' + v % 10); v /= 10; } while (v);
        while (i--) b[p++] = t[i];
        b[p++] = 's'; b[p++] = '\r'; b[p++] = '\n'; b[p] = 0;
        srv_puts(b);
    } else if (strcmp(cmd, "mem") == 0) {
        extern uint64_t pmm_free_bytes(void);
        char b[40];
        uint64_t kb = pmm_free_bytes() / 1024;
        int p = 0;
        char t[24];
        int i = 0;
        do { t[i++] = (char)('0' + kb % 10); kb /= 10; } while (kb);
        while (i--) b[p++] = t[i];
        const char *suf = " KiB liberi\r\n";
        while (*suf) b[p++] = *suf++;
        b[p] = 0;
        srv_puts(b);
    } else if (strcmp(cmd, "ls") == 0) {
        for (int i = 0; i < fs_count(); i++) {
            const struct fs_file *f = fs_get(i);
            srv_puts(f->name);
            srv_puts("\r\n");
        }
    } else if (strcmp(cmd, "cat") == 0) {
        uint32_t sz = 0;
        char *data = fs_read_file(arg, &sz);
        if (data) {
            for (uint32_t o = 0; o < sz; o += 512)
                srv_write(data + o, (int)(sz - o > 512 ? 512 : sz - o));
            kfree(data);
            srv_puts("\r\n");
        } else {
            srv_puts("nu exista\r\n");
        }
    } else if (strcmp(cmd, "exit") == 0) {
        srv_puts("La revedere!\r\n");
        tcp_out(&srv, F_FIN | F_ACK, 0, 0);
        srv.snd_nxt++;
        srv.state = CLOSED;
    } else if (cmd[0]) {
        srv_puts("comanda necunoscuta (scrie 'help')\r\n");
    }
    if (srv.state == ESTABLISHED)
        srv_puts("myos-net> ");
}

static void feed(const uint8_t *data, uint16_t len)
{
    for (uint16_t i = 0; i < len; i++) {
        char c = (char)data[i];
        if (c == '\r' || c == '\n') {
            if (line_len == 0 && c == '\n')
                continue;
            line[line_len] = '\0';
            srv_puts("\r\n");
            net_shell(line);
            line_len = 0;
        } else if (c == 8 || c == 127) {
            if (line_len > 0) {
                line_len--;
                srv_puts("\b \b");
            }
        } else if (c >= 32 && c < 127 && line_len < (int)sizeof(line) - 1) {
            line[line_len++] = c;
            srv_write(&c, 1);
        }
    }
}

static void srv_input(uint32_t src, const struct tcphdr *t,
                      const uint8_t *payload, uint16_t plen, uint32_t their_seq)
{
    uint8_t flags = t->flags;

    if (flags & F_RST) {
        srv.state = CLOSED;
        return;
    }
    if ((flags & F_SYN) && srv.state == CLOSED) {
        srv.rip = src;
        srv.rport = ntohs(t->sport);
        srv.lport = SRV_PORT;
        srv.rcv_nxt = their_seq + 1;
        srv.snd_nxt = 1000 + (uint32_t)pit_ticks();
        srv.state = SYN_RCVD;
        tcp_out(&srv, F_SYN | F_ACK, 0, 0);
        srv.snd_nxt++;
        return;
    }
    if (srv.state == SYN_RCVD && (flags & F_ACK)) {
        srv.state = ESTABLISHED;
        line_len = 0;
        srv_puts("\r\nBun venit la MyOS prin retea!\r\n");
        srv_puts("myos-net> ");
    }
    if (srv.state == ESTABLISHED && plen > 0 && their_seq == srv.rcv_nxt) {
        srv.rcv_nxt += plen;
        feed(payload, plen);
        tcp_out(&srv, F_ACK, 0, 0);
    }
    if (flags & F_FIN) {
        srv.rcv_nxt++;
        tcp_out(&srv, F_ACK, 0, 0);
        tcp_out(&srv, F_FIN | F_ACK, 0, 0);
        srv.snd_nxt++;
        srv.state = CLOSED;
    }
}

/* =====================================================================
 *  CLIENT outbound (active open) — pentru telnet/HTTP din MyOS
 * ===================================================================== */

#define NCLI 2
#define CLI_RX 16384
static struct tcp_conn cli[NCLI];
static uint16_t next_lport = 40000;

static void rx_push(struct tcp_conn *c, const uint8_t *data, int len)
{
    for (int i = 0; i < len; i++) {
        int nh = (c->rx_head + 1) % c->rx_cap;
        if (nh == c->rx_tail)
            break;                    /* buffer plin: aruncam surplusul */
        c->rx[c->rx_head] = data[i];
        c->rx_head = nh;
    }
}

int tcp_connect(uint32_t ip, uint16_t port)
{
    int h = -1;
    for (int i = 0; i < NCLI; i++)
        if (cli[i].state == CLOSED) { h = i; break; }
    if (h < 0)
        return -1;
    struct tcp_conn *c = &cli[h];
    if (!c->rx) {
        c->rx = (uint8_t *)kmalloc(CLI_RX);
        c->rx_cap = CLI_RX;
    }
    if (!c->rx)
        return -1;
    c->rx_head = c->rx_tail = 0;
    c->rip = ip;
    c->rport = port;
    c->lport = next_lport++;
    if (next_lport < 40000)
        next_lport = 40000;
    c->rcv_nxt = 0;
    c->snd_nxt = 2000 + (uint32_t)pit_ticks();
    c->state = SYN_SENT;
    c->t0 = pit_ticks();
    tcp_out(c, F_SYN, 0, 0);
    c->snd_nxt++;                     /* SYN consuma 1 */
    return h;
}

int tcp_status(int h)
{
    if (h < 0 || h >= NCLI)
        return 0;
    struct tcp_conn *c = &cli[h];
    if (c->state == SYN_SENT) {
        if (pit_ticks() - c->t0 > 500)   /* timeout 5s */
            c->state = CLOSED;
        else
            return 1;
    }
    if (c->state == ESTABLISHED)
        return 2;
    return 0;
}

int tcp_csend(int h, const void *buf, int len)
{
    if (h < 0 || h >= NCLI)
        return -1;
    struct tcp_conn *c = &cli[h];
    if (c->state != ESTABLISHED)
        return -1;
    if (len > 1400)
        len = 1400;
    tcp_out(c, F_PSH | F_ACK, buf, (uint16_t)len);
    c->snd_nxt += len;
    return len;
}

int tcp_crecv(int h, void *buf, int max)
{
    if (h < 0 || h >= NCLI)
        return -1;
    struct tcp_conn *c = &cli[h];
    uint8_t *out = (uint8_t *)buf;
    int n = 0;
    while (n < max && c->rx_tail != c->rx_head) {
        out[n++] = c->rx[c->rx_tail];
        c->rx_tail = (c->rx_tail + 1) % c->rx_cap;
    }
    return n;
}

void tcp_cclose(int h)
{
    if (h < 0 || h >= NCLI)
        return;
    struct tcp_conn *c = &cli[h];
    if (c->state == ESTABLISHED) {
        tcp_out(c, F_FIN | F_ACK, 0, 0);
        c->snd_nxt++;
    }
    c->state = CLOSED;
}

static void cli_input(struct tcp_conn *c, uint32_t src, const struct tcphdr *t,
                      const uint8_t *payload, uint16_t plen, uint32_t their_seq)
{
    (void)src;
    uint8_t flags = t->flags;

    if (flags & F_RST) {
        c->state = CLOSED;
        return;
    }
    if (c->state == SYN_SENT && (flags & F_SYN) && (flags & F_ACK)) {
        c->rcv_nxt = their_seq + 1;
        c->state = ESTABLISHED;
        tcp_out(c, F_ACK, 0, 0);      /* confirmam SYN-ul lor */
        return;
    }
    if (c->state == ESTABLISHED && plen > 0) {
        if (their_seq == c->rcv_nxt) {
            rx_push(c, payload, plen);
            c->rcv_nxt += plen;
            tcp_out(c, F_ACK, 0, 0);
        } else {
            /* out-of-order / retransmisie: reconfirmam ce asteptam */
            tcp_out(c, F_ACK, 0, 0);
        }
    }
    if (flags & F_FIN) {
        c->rcv_nxt++;
        tcp_out(c, F_ACK, 0, 0);
        /* raspundem cu FIN si inchidem (datele raman in rx pentru citire) */
        tcp_out(c, F_FIN | F_ACK, 0, 0);
        c->snd_nxt++;
        c->state = CLOSED;
    }
}

/* =====================================================================
 *  Dispatcher: alege serverul (port 23) sau conexiunea client potrivita
 * ===================================================================== */

static void tcp_input(uint32_t src, const uint8_t *seg, uint16_t len)
{
    if (len < 20)
        return;
    const struct tcphdr *t = (const struct tcphdr *)seg;
    uint16_t dport = ntohs(t->dport);
    uint32_t their_seq = ntohl(t->seq);
    int ihl = (t->off >> 4) * 4;
    if (ihl < 20 || ihl > len)
        return;
    const uint8_t *payload = seg + ihl;
    uint16_t plen = (uint16_t)(len - ihl);

    if (dport == SRV_PORT) {
        srv_input(src, t, payload, plen, their_seq);
        return;
    }
    for (int i = 0; i < NCLI; i++) {
        if (cli[i].state != CLOSED && cli[i].lport == dport &&
            cli[i].rip == src) {
            cli_input(&cli[i], src, t, payload, plen, their_seq);
            return;
        }
    }
}

void tcp_init(void)
{
    srv.state = CLOSED;
    for (int i = 0; i < NCLI; i++)
        cli[i].state = CLOSED;
    net_set_tcp(tcp_input);
    kprintf("[net] server telnet pe portul 23 (conecteaza-te la "
            "localhost:2323)\n");
}
