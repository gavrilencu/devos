#pragma once
#include <stdint.h>

/* Stiva de retea minimala: Ethernet + ARP + IPv4 + ICMP + (mai tarziu) TCP.
 * Adresa noastra IP e fixa (configuratia user-mode QEMU: 10.0.2.15). */

#define IP_ADDR   0x0A00020F   /* 10.0.2.15 */
#define IP_GW     0x0A000202   /* 10.0.2.2  */

void net_stack_init(void);

/* checksum standard de internet (complement fata de 1 pe 16 biti) */
uint16_t net_checksum(const void *data, int len);

/* trimite un pachet IPv4 (protocol: 1=ICMP, 6=TCP, 17=UDP) catre dst.
 * `payload` e continutul de dupa antetul IP. */
void ip_send(uint32_t dst, uint8_t proto, const void *payload, uint16_t len);

/* handler pentru pachetele TCP primite (setat de tcp.c) */
void net_set_tcp(void (*fn)(uint32_t src, const uint8_t *seg, uint16_t len));

/* ping (client ICMP echo). icmp_ping trimite o cerere; icmp_ping_result
 * intoarce: -1 = inca astept, -2 = timeout, altfel = RTT in ms. */
void icmp_ping(uint32_t ip);
int  icmp_ping_result(void);

/* UDP: trimite un datagram. Handler-ul de UDP e folosit intern de DNS. */
void udp_send(uint32_t dst, uint16_t sport, uint16_t dport,
              const void *payload, uint16_t len);

/* DNS (rezolvare nume). dns_query porneste o interogare; dns_result:
 * -1 astept, 0 esec/timeout, altfel IP-ul (ordinea gazdei). */
void dns_query(const char *name);
uint32_t dns_result(void);

/* "a.b.c.d" -> IP in ordinea gazdei; 0 daca nu e IP valid (deci e un nume) */
uint32_t ip_parse_k(const char *s);

#define IP_DNS 0x0A000203   /* 10.0.2.3 (serverul DNS al QEMU) */

void tcp_init(void);            /* porneste serverul telnet (tcp.c) */

/* Client TCP outbound (active open). Toate sunt non-blocante: syscall-urile
 * ruleaza cu intreruperile oprite, deci userspace face polling (ca readc). */
int  tcp_connect(uint32_t ip, uint16_t port); /* -> handle 0..N sau -1 */
int  tcp_status(int h);         /* 0=inchis, 1=conectare, 2=stabilit */
int  tcp_csend(int h, const void *buf, int len);
int  tcp_crecv(int h, void *buf, int max);     /* octeti cititi (0=nimic) */
void tcp_cclose(int h);
