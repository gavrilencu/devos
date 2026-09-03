#pragma once
#include <stdint.h>

/* Interfata generica de retea (deocamdata un singur driver: RTL8139). */

#define ETH_ALEN 6

/* MAC-ul placii noastre (completat de driver la init). */
extern uint8_t net_mac[ETH_ALEN];

/* Initializeaza placa de retea. 1 la succes, 0 daca nu exista. */
int net_init(void);
int net_up(void);                    /* 1 daca placa e initializata */

/* Trimite un cadru Ethernet complet (deja cu antetul, incepand cu MAC dst). */
void net_send(const void *frame, uint16_t len);

/* Cadrele primite sunt predate acestui handler (setat de stiva IP). */
void net_set_rx(void (*fn)(const uint8_t *frame, uint16_t len));
