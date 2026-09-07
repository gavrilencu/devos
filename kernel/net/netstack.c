#include "netstack.h"
#include "net.h"
#include "pit.h"
#include "string.h"
#include "kprintf.h"
/* ---- structuri de protocol (toate big-endian pe fir) ---- */

static uint16_t bswap16(uint16_t v) { return (uint16_t)((v << 8) | (v >> 8)); }
static uint32_t bswap32(uint32_t v)
{
    return (v << 24) | ((v & 0xFF00) << 8) | ((v >> 8) & 0xFF00) | (v >> 24);
}
#define htons(x) bswap16(x)
#define ntohs(x) bswap16(x)
#define htonl(x) bswap32(x)
#define ntohl(x) bswap32(x)

#define ET_ARP 0x0806
#define ET_IP  0x0800

struct eth {
    uint8_t dst[6];
    uint8_t src[6];
    uint16_t type;
} __attribute__((packed));

struct arp {
    uint16_t htype, ptype;
    uint8_t hlen, plen;
    uint16_t op;
    uint8_t sha[6];
    uint32_t spa;
    uint8_t tha[6];
    uint32_t tpa;
} __attribute__((packed));

struct ip4 {
    uint8_t ver_ihl, tos;
    uint16_t len, id, frag;
    uint8_t ttl, proto;
    uint16_t csum;
    uint32_t src, dst;
} __attribute__((packed));

struct icmp {
    uint8_t type, code;
    uint16_t csum;
    uint16_t id, seq;
} __attribute__((packed));

/* cache ARP simplu: doar gateway-ul (destul pentru user-mode QEMU) */
static uint8_t gw_mac[6];
static int gw_known;

static void (*tcp_handler)(uint32_t src, const uint8_t *seg, uint16_t len);

void net_set_tcp(void (*fn)(uint32_t src, const uint8_t *seg, uint16_t len))
{
    tcp_handler = fn;
}

uint16_t net_checksum(const void *data, int len)
{
    const uint8_t *p = data;
    uint32_t sum = 0;
    while (len > 1) {
        sum += (uint32_t)(p[0] << 8 | p[1]);
        p += 2;
        len -= 2;
    }
    if (len)
        sum += (uint32_t)(p[0] << 8);
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

/* ---- trimitere ---- */

static void eth_send(const uint8_t dst[6], uint16_t type,
                     const void *payload, uint16_t len)
{
    static uint8_t frame[1600];
    struct eth *e = (struct eth *)frame;
    memcpy(e->dst, dst, 6);
    memcpy(e->src, net_mac, 6);
    e->type = htons(type);
    memcpy(frame + sizeof(*e), payload, len);
    net_send(frame, (uint16_t)(sizeof(*e) + len));
}

static void arp_request(uint32_t tpa)
{
    struct arp a;
    a.htype = htons(1);
    a.ptype = htons(ET_IP);
    a.hlen = 6;
    a.plen = 4;
    a.op = htons(1);           /* request */
    memcpy(a.sha, net_mac, 6);
    a.spa = htonl(IP_ADDR);
    memset(a.tha, 0, 6);
    a.tpa = htonl(tpa);
    uint8_t bcast[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    eth_send(bcast, ET_ARP, &a, sizeof(a));
}

void ip_send(uint32_t dst, uint8_t proto, const void *payload, uint16_t len)
{
    static uint8_t pkt[1600];
    struct ip4 *ip = (struct ip4 *)pkt;
    ip->ver_ihl = 0x45;
    ip->tos = 0;
    ip->len = htons((uint16_t)(sizeof(*ip) + len));
    ip->id = 0;
    ip->frag = 0;
    ip->ttl = 64;
    ip->proto = proto;
    ip->csum = 0;
    ip->src = htonl(IP_ADDR);
    ip->dst = htonl(dst);
    ip->csum = htons(net_checksum(ip, sizeof(*ip)));
    memcpy(pkt + sizeof(*ip), payload, len);

    /* destinatia locala trece prin gateway (user-mode QEMU) */
    const uint8_t *dmac = gw_known ? gw_mac : 0;
    if (!dmac) {
        arp_request(IP_GW);    /* invatam MAC-ul; pachetul asta se pierde */
        return;
    }
    eth_send(dmac, ET_IP, pkt, (uint16_t)(sizeof(*ip) + len));
}

/* ---- receptie ---- */

static void handle_arp(const struct arp *a)
{
    if (ntohs(a->op) == 2) {          /* reply: retinem MAC-ul gateway-ului */
        if (ntohl(a->spa) == IP_GW) {
            memcpy(gw_mac, a->sha, 6);
            gw_known = 1;
        }
        return;
    }
    if (ntohs(a->op) == 1 && ntohl(a->tpa) == IP_ADDR) {
        struct arp r;              /* raspundem la cine ne cauta */
        r.htype = htons(1);
        r.ptype = htons(ET_IP);
        r.hlen = 6;
        r.plen = 4;
        r.op = htons(2);
        memcpy(r.sha, net_mac, 6);
        r.spa = htonl(IP_ADDR);
        memcpy(r.tha, a->sha, 6);
        r.tpa = a->spa;
        eth_send(a->sha, ET_ARP, &r, sizeof(r));
    }
}

/* ---- ping (client ICMP echo) ---- */
#define PING_ID 0x4D59                /* 'MY' */
static volatile uint64_t ping_tick;
static volatile int ping_got;
static uint16_t ping_seq;

void icmp_ping(uint32_t ip)
{
    ping_got = 0;
    ping_tick = pit_ticks();
    ping_seq++;
    struct icmp ic;
    ic.type = 8;                      /* echo request */
    ic.code = 0;
    ic.csum = 0;
    ic.id = htons(PING_ID);
    ic.seq = htons(ping_seq);
    ic.csum = htons(net_checksum(&ic, sizeof(ic)));
    ip_send(ip, 1, &ic, sizeof(ic));
}

int icmp_ping_result(void)
{
    if (ping_got)
        return (int)((pit_ticks() - ping_tick) * 10);   /* 10 ms/tick */
    if (pit_ticks() - ping_tick > 300)                   /* 3 s timeout */
        return -2;
    return -1;
}

static void handle_icmp(uint32_t src, const struct icmp *ic, uint16_t len)
{
    if (ic->type == 0) {              /* echo reply: raspuns la ping-ul nostru */
        if (ntohs(ic->id) == PING_ID)
            ping_got = 1;
        return;
    }
    if (ic->type != 8)                /* altfel doar echo request */
        return;
    static uint8_t buf[1600];
    struct icmp *r = (struct icmp *)buf;
    memcpy(r, ic, len);
    r->type = 0;                      /* echo reply */
    r->csum = 0;
    r->csum = htons(net_checksum(r, len));
    ip_send(src, 1, r, len);
}

/* ---- UDP + DNS ---- */

struct udphdr {
    uint16_t sport, dport, len, csum;
} __attribute__((packed));

void udp_send(uint32_t dst, uint16_t sport, uint16_t dport,
              const void *payload, uint16_t len)
{
    static uint8_t buf[1600];
    struct udphdr *u = (struct udphdr *)buf;
    u->sport = htons(sport);
    u->dport = htons(dport);
    u->len = htons((uint16_t)(sizeof(*u) + len));
    u->csum = 0;                      /* checksum optional in IPv4 */
    memcpy(buf + sizeof(*u), payload, len);
    ip_send(dst, 17, buf, (uint16_t)(sizeof(*u) + len));
}

/* stare DNS (o interogare simultan) */
static uint16_t dns_id;
static volatile uint32_t dns_ip;
static volatile int dns_state;        /* 0 idle/gata, 1 astept, 2 esec */
static uint64_t dns_tick;

void dns_query(const char *name)
{
    static uint8_t q[512];
    int p = 0;
    dns_id++;
    q[p++] = (uint8_t)(dns_id >> 8);
    q[p++] = (uint8_t)dns_id;
    q[p++] = 0x01; q[p++] = 0x00;     /* flags: recursion desired */
    q[p++] = 0x00; q[p++] = 0x01;     /* qdcount = 1 */
    q[p++] = 0; q[p++] = 0;           /* ancount */
    q[p++] = 0; q[p++] = 0;           /* nscount */
    q[p++] = 0; q[p++] = 0;           /* arcount */

    /* qname: fiecare eticheta precedata de lungimea ei */
    int i = 0;
    while (name[i]) {
        int start = i;
        while (name[i] && name[i] != '.')
            i++;
        int llen = i - start;
        q[p++] = (uint8_t)llen;
        for (int k = 0; k < llen; k++)
            q[p++] = (uint8_t)name[start + k];
        if (name[i] == '.')
            i++;
    }
    q[p++] = 0;                       /* radacina */
    q[p++] = 0x00; q[p++] = 0x01;     /* qtype = A */
    q[p++] = 0x00; q[p++] = 0x01;     /* qclass = IN */

    dns_ip = 0;
    dns_state = 1;
    dns_tick = pit_ticks();
    udp_send(IP_DNS, 0xC000, 53, q, (uint16_t)p);
}

uint32_t ip_parse_k(const char *s)
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

uint32_t dns_result(void)
{
    if (dns_state == 1 && pit_ticks() - dns_tick > 400)
        dns_state = 2;                /* timeout 4s */
    if (dns_state == 1)
        return (uint32_t)-1;          /* inca astept */
    if (dns_state == 2)
        return 0;                     /* esec */
    return dns_ip;
}

/* sare peste un nume DNS (eticheta/lungime sau pointer de compresie) */
static int dns_skip_name(const uint8_t *msg, int off, int end)
{
    while (off < end) {
        uint8_t l = msg[off];
        if (l == 0)
            return off + 1;
        if ((l & 0xC0) == 0xC0)
            return off + 2;           /* pointer de compresie: 2 bytes */
        off += l + 1;
    }
    return end;
}

static void handle_dns(const uint8_t *msg, int len)
{
    if (len < 12)
        return;
    uint16_t id = (uint16_t)(msg[0] << 8 | msg[1]);
    if (id != dns_id || dns_state != 1)
        return;
    int qd = msg[4] << 8 | msg[5];
    int an = msg[6] << 8 | msg[7];
    int off = 12;
    for (int i = 0; i < qd; i++) {
        off = dns_skip_name(msg, off, len);
        off += 4;                     /* qtype + qclass */
    }
    for (int i = 0; i < an && off + 10 <= len; i++) {
        off = dns_skip_name(msg, off, len);
        int type = msg[off] << 8 | msg[off + 1];
        int rdlen = msg[off + 8] << 8 | msg[off + 9];
        int rd = off + 10;
        if (type == 1 && rdlen == 4 && rd + 4 <= len) {
            dns_ip = (uint32_t)(msg[rd] << 24 | msg[rd + 1] << 16 |
                                msg[rd + 2] << 8 | msg[rd + 3]);
            dns_state = 0;            /* gata */
            return;
        }
        off = rd + rdlen;
    }
    dns_state = 2;                    /* raspuns fara A record */
}

static void handle_udp(const struct udphdr *u, uint16_t plen)
{
    if (plen < sizeof(*u))
        return;
    uint16_t sport = ntohs(u->sport);
    const uint8_t *data = (const uint8_t *)u + sizeof(*u);
    int dlen = (int)ntohs(u->len) - (int)sizeof(*u);
    if (dlen < 0 || dlen > plen - (int)sizeof(*u))
        return;
    if (sport == 53)
        handle_dns(data, dlen);
}

static void handle_ip(const struct ip4 *ip, uint16_t caplen)
{
    if ((ip->ver_ihl >> 4) != 4)
        return;
    int ihl = (ip->ver_ihl & 0x0F) * 4;
    uint16_t total = ntohs(ip->len);
    if (total > caplen || ihl < 20)
        return;
    const uint8_t *payload = (const uint8_t *)ip + ihl;
    uint16_t plen = (uint16_t)(total - ihl);
    uint32_t src = ntohl(ip->src);

    if (ip->proto == 1)
        handle_icmp(src, (const struct icmp *)payload, plen);
    else if (ip->proto == 17)
        handle_udp((const struct udphdr *)payload, plen);
    else if (ip->proto == 6 && tcp_handler)
        tcp_handler(src, payload, plen);
}

static void on_frame(const uint8_t *frame, uint16_t len)
{
    if (len < sizeof(struct eth))
        return;
    const struct eth *e = (const struct eth *)frame;
    uint16_t type = ntohs(e->type);
    const uint8_t *payload = frame + sizeof(*e);
    uint16_t plen = (uint16_t)(len - sizeof(*e));

    if (type == ET_ARP && plen >= sizeof(struct arp))
        handle_arp((const struct arp *)payload);
    else if (type == ET_IP && plen >= sizeof(struct ip4))
        handle_ip((const struct ip4 *)payload, plen);
}

void net_stack_init(void)
{
    net_set_rx(on_frame);
    arp_request(IP_GW);      /* aflam din start MAC-ul gateway-ului */
    kprintf("[net] stiva IP pornita: 10.0.2.15, gateway 10.0.2.2\n");
}
