#pragma once
#include <stdint.h>

/* Client TLS 1.2 (ECDHE-x25519 + AES-128-GCM). Ruleaza peste clientul TCP
 * din tcp.c, pe firul de kernel al browserului (poate face task_sleep).
 * NU valideaza certificatul (accepta orice server) — potrivit pentru un OS
 * de invatare, NU pentru securitate reala.
 *
 * tls_connect face tot handshake-ul (blocant, cu polling) si intoarce un
 * handle sau -1. tls_send/tls_recv cripteaza/decripteaza inregistrarile.
 * tls_recv: >0 octeti, -1 inchis/eroare. */

int  tls_connect(uint32_t ip, uint16_t port, const char *hostname);
int  tls_send(int h, const void *buf, int len);
int  tls_recv(int h, void *buf, int max);
void tls_close(int h);
