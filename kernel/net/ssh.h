#pragma once
#include <stdint.h>

/* Client SSH-2 de la zero (RFC 4253/4252/4254). Reutilizeaza criptografia
 * proprie: x25519 (KEX curve25519-sha256), AES-128-CTR (cifru),
 * HMAC-SHA256 (MAC), SHA-256 (hash de schimb + derivare chei).
 * O singura sesiune; ruleaza pe un fir de kernel (poate task_sleep).
 * NU verifica cheia gazdei (OS de invatare). Autentificare: parola. */

/* Porneste o sesiune SSH catre ip:port cu user/parola. Intoarce 0 la start
 * (handshake-ul ruleaza in fundal), -1 daca deja e una activa. */
int  ssh_open(uint32_t ip, uint16_t port, const char *user, const char *pass);

/* 0 = inactiv/inchis, 1 = in curs (conectare/handshake/auth),
 * 2 = gata (shell interactiv), -1 = eroare. */
int  ssh_status(void);

/* Citeste date primite de la server (iesirea shell-ului). Octeti cititi (0=nimic). */
int  ssh_read(void *buf, int max);

/* Trimite date catre server (tastele utilizatorului). */
int  ssh_write(const void *buf, int len);

/* Inchide sesiunea. */
void ssh_close(void);

/* Mesaj de stare/eroare pentru afisare. */
const char *ssh_error(void);

/* Genereaza o pereche noua de chei Ed25519 si o salveaza pe disc
 * (fisierul "id_ed25519"). Intoarce 0 la succes, -1 la eroare. */
int  ssh_keygen(void);

/* Scrie linia pentru authorized_keys ("ssh-ed25519 <base64> devos@devos")
 * in buf. Intoarce lungimea, sau -1 daca nu exista cheie. */
int  ssh_get_pubkey_line(char *buf, int max);
