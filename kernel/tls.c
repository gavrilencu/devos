/* Client TLS 1.2: ECDHE (x25519) + AES-128-GCM + SHA-256.
 * Fara validare de certificat. Vezi tls.h. */

#include "tls.h"
#include "netstack.h"
#include "sha256.h"
#include "aes.h"
#include "x25519.h"
#include "pit.h"
#include "pmm.h"
#include "string.h"
#include "task.h"
#include "serial.h"

#define TLS_DEBUG 0
#if TLS_DEBUG
#define TDBG(x) serial_write("[tls] " x "\n")
#else
#define TDBG(x)
#endif

#define REC_MAX 16640          /* 5 + 16384 + spatiu */

struct tls {
    int used;
    int tcp;
    int established;
    int closed;

    uint8_t client_random[32];
    uint8_t server_random[32];
    uint8_t server_pub[32];
    uint8_t priv[32];
    uint8_t master[48];

    uint8_t cwk[16], swk[16];      /* chei de scriere client/server */
    uint8_t civ[4], siv[4];        /* IV impliciti */
    uint64_t cseq, sseq;

    sha256_ctx hsh;                /* hash-ul mesajelor de handshake */

    uint8_t rxbuf[REC_MAX + 4096]; /* reasamblare TCP */
    int rxlen;

    uint8_t hs[REC_MAX + 8192];    /* reasamblare mesaje handshake */
    int hslen, hspos;

    uint8_t app[REC_MAX];          /* date aplicatie decriptate */
    int applen, apppos;

    uint8_t rec[REC_MAX];          /* buffer de lucru pt. o inregistrare */
    uint8_t plain[REC_MAX];        /* text clar de lucru */
};

static struct tls *S;              /* o singura sesiune (browserul face una) */

/* ---- RNG slab (nu criptografic; suficient functional) ---- */
static uint64_t rng_state;
static uint64_t rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}
static void tls_random(uint8_t *buf, int n)
{
    if (rng_state == 0)
        rng_state = rdtsc() ^ (pit_ticks() * 2654435761u) ^ 0x9e3779b97f4a7c15ull;
    for (int i = 0; i < n; i++) {
        rng_state ^= rng_state << 13;
        rng_state ^= rng_state >> 7;
        rng_state ^= rng_state << 17;
        rng_state += rdtsc();
        buf[i] = (uint8_t)(rng_state >> 24);
    }
}

/* ---- PRF TLS 1.2 (P_SHA256) ---- */
static void tls_prf(const uint8_t *secret, int seclen,
                    const char *label, const uint8_t *seed, int seedlen,
                    uint8_t *out, int outlen)
{
    uint8_t ls[128];             /* label || seed */
    int ll = 0;
    for (const char *p = label; *p; p++) ls[ll++] = (uint8_t)*p;
    memcpy(ls + ll, seed, seedlen);
    ll += seedlen;

    uint8_t a[32];
    hmac_sha256(secret, seclen, ls, ll, a);      /* A(1) */
    int done = 0;
    while (done < outlen) {
        uint8_t inbuf[32 + 128];
        memcpy(inbuf, a, 32);
        memcpy(inbuf + 32, ls, ll);
        uint8_t block[32];
        hmac_sha256(secret, seclen, inbuf, 32 + ll, block);
        int n = outlen - done < 32 ? outlen - done : 32;
        memcpy(out + done, block, n);
        done += n;
        hmac_sha256(secret, seclen, a, 32, a);   /* A(i+1) */
    }
}

/* ---- TCP jos ---- */
static int tls_tcp_fill(struct tls *s, int need)
{
    int idle = 0;
    while (s->rxlen < need) {
        int cap = (int)sizeof(s->rxbuf) - s->rxlen;
        if (cap <= 0) return -1;
        int r = tcp_crecv(s->tcp, s->rxbuf + s->rxlen, cap > 1400 ? 1400 : cap);
        if (r > 0) { s->rxlen += r; idle = 0; continue; }
        if (tcp_status(s->tcp) == 0) return -1;
        if (++idle > 500) return -1;      /* ~10s */
        task_sleep(20);
    }
    return 0;
}

static void tls_tcp_send_all(struct tls *s, const uint8_t *data, int len)
{
    int off = 0, idle = 0;
    while (off < len) {
        int n = tcp_csend(s->tcp, data + off, len - off);
        if (n > 0) { off += n; idle = 0; continue; }
        if (tcp_status(s->tcp) == 0) return;
        if (++idle > 500) return;
        task_sleep(10);
    }
}

/* citeste o inregistrare TLS: type + payload in `out` (max REC_MAX) */
static int tls_read_record(struct tls *s, int *type, uint8_t *out, int *plen)
{
    if (tls_tcp_fill(s, 5) < 0) return -1;
    *type = s->rxbuf[0];
    int len = (s->rxbuf[3] << 8) | s->rxbuf[4];
    if (len < 0 || len > REC_MAX) return -1;
    if (tls_tcp_fill(s, 5 + len) < 0) return -1;
    memcpy(out, s->rxbuf + 5, len);
    *plen = len;
    memmove(s->rxbuf, s->rxbuf + 5 + len, s->rxlen - (5 + len));
    s->rxlen -= (5 + len);
    return 0;
}

/* trimite o inregistrare in clar (handshake pre-CCS, sau CCS) */
static void tls_write_plain(struct tls *s, int type, const uint8_t *data, int len)
{
    uint8_t hdr[5] = { (uint8_t)type, 0x03, 0x03,
                       (uint8_t)(len >> 8), (uint8_t)len };
    tls_tcp_send_all(s, hdr, 5);
    if (len) tls_tcp_send_all(s, data, len);
}

static void put_seq(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (56 - i * 8));
}

/* trimite o inregistrare criptata AES-GCM */
static void tls_write_enc(struct tls *s, int type, const uint8_t *data, int len)
{
    uint8_t nonce[12], aad[13];
    memcpy(nonce, s->civ, 4);
    put_seq(nonce + 4, s->cseq);         /* nonce explicit = numarul de secventa */
    put_seq(aad, s->cseq);
    aad[8] = (uint8_t)type; aad[9] = 0x03; aad[10] = 0x03;
    aad[11] = (uint8_t)(len >> 8); aad[12] = (uint8_t)len;

    uint8_t *rec = s->rec;
    memcpy(rec + 5, nonce + 4, 8);       /* nonce explicit pe fir */
    uint8_t *cipher = rec + 5 + 8;
    uint8_t tag[16];
    aes_gcm_encrypt(s->cwk, 128, nonce, aad, 13, data, len, cipher, tag);
    memcpy(cipher + len, tag, 16);
    int reclen = 8 + len + 16;
    rec[0] = (uint8_t)type; rec[1] = 0x03; rec[2] = 0x03;
    rec[3] = (uint8_t)(reclen >> 8); rec[4] = (uint8_t)reclen;
    tls_tcp_send_all(s, rec, 5 + reclen);
    s->cseq++;
}

/* decripteaza o inregistrare (buf contine nonce_explicit||cipher||tag) */
static int tls_decrypt(struct tls *s, int type, uint8_t *buf, int *len)
{
    if (*len < 8 + 16) return -1;
    int clen = *len - 8 - 16;
    uint8_t nonce[12], aad[13];
    memcpy(nonce, s->siv, 4);
    memcpy(nonce + 4, buf, 8);
    put_seq(aad, s->sseq);
    aad[8] = (uint8_t)type; aad[9] = 0x03; aad[10] = 0x03;
    aad[11] = (uint8_t)(clen >> 8); aad[12] = (uint8_t)clen;
    if (aes_gcm_decrypt(s->swk, 128, nonce, aad, 13,
                        buf + 8, clen, buf + 8 + clen, s->plain) != 0)
        return -1;
    memcpy(buf, s->plain, clen);
    *len = clen;
    s->sseq++;
    return 0;
}

/* ---- construirea ClientHello ---- */
static int build_client_hello(struct tls *s, const char *host, uint8_t *out)
{
    int p = 0;
    uint8_t *b = out;

    /* corpul ClientHello (fara antetul de handshake) */
    b[p++] = 0x03; b[p++] = 0x03;          /* client_version TLS 1.2 */
    memcpy(b + p, s->client_random, 32); p += 32;
    b[p++] = 0x00;                         /* session_id lung 0 */

    /* cipher suites */
    b[p++] = 0x00; b[p++] = 0x04;
    b[p++] = 0xc0; b[p++] = 0x2f;          /* ECDHE_RSA_AES128_GCM_SHA256 */
    b[p++] = 0xc0; b[p++] = 0x2b;          /* ECDHE_ECDSA_AES128_GCM_SHA256 */

    b[p++] = 0x01; b[p++] = 0x00;          /* compression: null */

    /* extensii */
    int extlen_pos = p; p += 2;
    int ext_start = p;

    /* SNI */
    int hlen = 0; while (host[hlen]) hlen++;
    b[p++] = 0x00; b[p++] = 0x00;                          /* type server_name */
    b[p++] = 0x00; b[p++] = (uint8_t)(hlen + 5);           /* ext len */
    b[p++] = 0x00; b[p++] = (uint8_t)(hlen + 3);           /* list len */
    b[p++] = 0x00;                                         /* name_type host */
    b[p++] = 0x00; b[p++] = (uint8_t)hlen;                 /* name len */
    memcpy(b + p, host, hlen); p += hlen;

    /* supported_groups: x25519 */
    b[p++] = 0x00; b[p++] = 0x0a;
    b[p++] = 0x00; b[p++] = 0x04;
    b[p++] = 0x00; b[p++] = 0x02;
    b[p++] = 0x00; b[p++] = 0x1d;

    /* ec_point_formats: uncompressed */
    b[p++] = 0x00; b[p++] = 0x0b;
    b[p++] = 0x00; b[p++] = 0x02;
    b[p++] = 0x01; b[p++] = 0x00;

    /* signature_algorithms */
    static const uint8_t sigs[] = {
        0x04,0x03, 0x04,0x01, 0x05,0x03, 0x05,0x01,
        0x06,0x03, 0x06,0x01, 0x08,0x04, 0x08,0x05, 0x02,0x01
    };
    b[p++] = 0x00; b[p++] = 0x0d;
    b[p++] = 0x00; b[p++] = (uint8_t)(sizeof(sigs) + 2);
    b[p++] = 0x00; b[p++] = (uint8_t)sizeof(sigs);
    memcpy(b + p, sigs, sizeof(sigs)); p += sizeof(sigs);

    /* renegotiation_info (gol) */
    b[p++] = 0xff; b[p++] = 0x01;
    b[p++] = 0x00; b[p++] = 0x01;
    b[p++] = 0x00;

    int ext_len = p - ext_start;
    b[extlen_pos] = (uint8_t)(ext_len >> 8);
    b[extlen_pos + 1] = (uint8_t)ext_len;

    return p;
}

/* adauga un mesaj de handshake in hash (bytes = type||len24||body) */
static void hs_hash(struct tls *s, const uint8_t *msg, int len)
{
    sha256_update(&s->hsh, msg, len);
}

/* trimite un mesaj de handshake (in clar) si il adauga la hash */
static void send_handshake(struct tls *s, int hstype, const uint8_t *body, int blen)
{
    uint8_t hdr[4] = { (uint8_t)hstype, (uint8_t)(blen >> 16),
                       (uint8_t)(blen >> 8), (uint8_t)blen };
    /* hash: header + body */
    sha256_update(&s->hsh, hdr, 4);
    if (blen) sha256_update(&s->hsh, body, blen);
    /* record: le trimitem impreuna */
    static uint8_t tmp[REC_MAX];
    memcpy(tmp, hdr, 4);
    if (blen) memcpy(tmp + 4, body, blen);
    tls_write_plain(s, 22, tmp, 4 + blen);
}

/* proceseaza mesajele de handshake din bufferul s->hs pana la
 * ServerHelloDone. Intoarce 0 la ServerHelloDone, -1 eroare, 1 mai citeste */
static int process_handshake_msgs(struct tls *s, int *done)
{
    while (s->hslen - s->hspos >= 4) {
        uint8_t *m = s->hs + s->hspos;
        int mtype = m[0];
        int mlen = (m[1] << 16) | (m[2] << 8) | m[3];
        if (s->hslen - s->hspos < 4 + mlen)
            return 1;                        /* mesaj incomplet, mai citim */

        hs_hash(s, m, 4 + mlen);             /* toate intra in hash */
        uint8_t *body = m + 4;

        if (mtype == 2) {                    /* ServerHello */
            /* version(2) + random(32) + sid_len(1) + sid + cipher(2) + comp(1) + ext */
            memcpy(s->server_random, body + 2, 32);
            int sidlen = body[34];
            int off = 35 + sidlen;
            int cipher = (body[off] << 8) | body[off + 1];
#if TLS_DEBUG
            { char h[8]; static const char *hx = "0123456789abcdef";
              h[0]='c'; h[1]='='; h[2]=hx[(cipher>>12)&15]; h[3]=hx[(cipher>>8)&15];
              h[4]=hx[(cipher>>4)&15]; h[5]=hx[cipher&15]; h[6]='\n'; h[7]=0;
              serial_write("[tls] serverhello "); serial_write(h); }
#endif
            if (cipher != 0xc02f && cipher != 0xc02b) {
                TDBG("cipher not supported");
                return -1;                   /* suita neacceptata */
            }
        } else if (mtype == 12) {            /* ServerKeyExchange */
            /* curve_type(1)=3 + named_curve(2)=001d + pub_len(1) + pub(32) + sig */
            if (body[0] != 3) return -1;
            int nc = (body[1] << 8) | body[2];
            if (nc != 0x001d) return -1;     /* nu e x25519 */
            int publen = body[3];
            if (publen != 32) return -1;
            memcpy(s->server_pub, body + 4, 32);
        } else if (mtype == 14) {            /* ServerHelloDone */
            TDBG("serverhellodone");
            *done = 1;
            s->hspos += 4 + mlen;
            return 0;
        }
        /* Certificate(11), CertificateRequest(13): doar hash, ignoram */

        s->hspos += 4 + mlen;
    }
    return 1;
}

int tls_connect(uint32_t ip, uint16_t port, const char *hostname)
{
    if (!S) {
        uint64_t phys = pmm_alloc_contig((sizeof(struct tls) + PMM_FRAME_SIZE - 1)
                                         / PMM_FRAME_SIZE);
        if (!phys) return -1;
        S = (struct tls *)phys;
    }
    struct tls *s = S;
    memset(s, 0, sizeof(*s));

    s->tcp = tcp_connect(ip, port);
    if (s->tcp < 0) return -1;
    int st, idle = 0;
    while ((st = tcp_status(s->tcp)) == 1) {
        if (++idle > 500) { tcp_cclose(s->tcp); return -1; }
        task_sleep(20);
    }
    if (st != 2) { tcp_cclose(s->tcp); TDBG("tcp connect fail"); return -1; }
    TDBG("tcp ok");

    sha256_init(&s->hsh);
    tls_random(s->client_random, 32);
    tls_random(s->priv, 32);

    /* --- ClientHello --- */
    static uint8_t chbody[600];
    int chlen = build_client_hello(s, hostname, chbody);
    send_handshake(s, 1, chbody, chlen);
    TDBG("clienthello sent");

    /* --- citim pana la ServerHelloDone --- */
    s->hslen = 0; s->hspos = 0;
    int done = 0;
    while (!done) {
        int type, len = 0;
        static uint8_t rp[REC_MAX];
        if (tls_read_record(s, &type, rp, &len) < 0) { tcp_cclose(s->tcp); return -1; }
        if (type == 22) {
            if (s->hslen + len > (int)sizeof(s->hs)) { tcp_cclose(s->tcp); return -1; }
            memcpy(s->hs + s->hslen, rp, len);
            s->hslen += len;
            int r = process_handshake_msgs(s, &done);
            if (r < 0) { tcp_cclose(s->tcp); return -1; }
        } else if (type == 21) {             /* alert */
            tcp_cclose(s->tcp); return -1;
        }
    }

    /* --- ECDH: calculam secretul comun --- */
    uint8_t shared[32], clientpub[32];
    x25519_base(clientpub, s->priv);
    x25519(shared, s->priv, s->server_pub);

    /* master_secret = PRF(pre_master, "master secret", cr+sr, 48) */
    uint8_t seed[64];
    memcpy(seed, s->client_random, 32);
    memcpy(seed + 32, s->server_random, 32);
    tls_prf(shared, 32, "master secret", seed, 64, s->master, 48);

    /* key_block = PRF(master, "key expansion", sr+cr, 40) */
    memcpy(seed, s->server_random, 32);
    memcpy(seed + 32, s->client_random, 32);
    uint8_t kb[40];
    tls_prf(s->master, 48, "key expansion", seed, 64, kb, 40);
    memcpy(s->cwk, kb, 16);
    memcpy(s->swk, kb + 16, 16);
    memcpy(s->civ, kb + 32, 4);
    memcpy(s->siv, kb + 36, 4);
    s->cseq = 0; s->sseq = 0;

    /* --- ClientKeyExchange --- */
    uint8_t cke[33];
    cke[0] = 32;
    memcpy(cke + 1, clientpub, 32);
    send_handshake(s, 16, cke, 33);

    /* --- ChangeCipherSpec --- */
    uint8_t ccs = 0x01;
    tls_write_plain(s, 20, &ccs, 1);

    /* --- Finished (criptat) --- */
    /* verify_data = PRF(master, "client finished", SHA256(handshake), 12) */
    sha256_ctx tmp = s->hsh;                 /* clona: nu consumam originalul */
    uint8_t hshash[32];
    sha256_final(&tmp, hshash);
    uint8_t vd[12];
    tls_prf(s->master, 48, "client finished", hshash, 32, vd, 12);
    uint8_t fin[16];
    fin[0] = 20; fin[1] = 0; fin[2] = 0; fin[3] = 12;
    memcpy(fin + 4, vd, 12);
    /* adaugam Finished-ul nostru la hash (pt. verificarea serverului) */
    sha256_update(&s->hsh, fin, 16);
    tls_write_enc(s, 22, fin, 16);
    TDBG("client finished sent");

    /* --- asteptam CCS + Finished de la server --- */
    int got_ccs = 0, got_fin = 0, tries = 0;
    while (!got_fin && tries++ < 20) {
        int type, len = 0;
        static uint8_t rp[REC_MAX];
        if (tls_read_record(s, &type, rp, &len) < 0) { tcp_cclose(s->tcp); return -1; }
        if (type == 20) {                    /* ChangeCipherSpec */
            got_ccs = 1;
        } else if (type == 22 && got_ccs) {  /* Finished criptat */
            if (tls_decrypt(s, 22, rp, &len) < 0) { tcp_cclose(s->tcp); return -1; }
            got_fin = 1;                      /* nu verificam continutul */
        } else if (type == 21) {
            tcp_cclose(s->tcp); return -1;
        }
    }
    if (!got_fin) { tcp_cclose(s->tcp); TDBG("no server finished"); return -1; }
    TDBG("established");

    s->used = 1;
    s->established = 1;
    s->closed = 0;
    s->applen = 0; s->apppos = 0;
    return 0;                                 /* un singur handle: 0 */
}

int tls_send(int h, const void *buf, int len)
{
    (void)h;
    struct tls *s = S;
    if (!s || !s->established) return -1;
    const uint8_t *p = (const uint8_t *)buf;
    int off = 0;
    while (off < len) {
        int n = len - off;
        if (n > 16000) n = 16000;
        tls_write_enc(s, 23, p + off, n);
        off += n;
    }
    return len;
}

int tls_recv(int h, void *buf, int max)
{
    (void)h;
    struct tls *s = S;
    if (!s || !s->established) return -1;

    /* mai avem date decriptate in buffer? */
    if (s->apppos < s->applen) {
        int n = s->applen - s->apppos;
        if (n > max) n = max;
        memcpy(buf, s->app + s->apppos, n);
        s->apppos += n;
        return n;
    }
    if (s->closed) return -1;

    /* citim si decriptam urmatoarea inregistrare cu date */
    for (;;) {
        int type, len = 0;
        static uint8_t rp[REC_MAX];
        if (tls_read_record(s, &type, rp, &len) < 0) { s->closed = 1; return -1; }
        if (tls_decrypt(s, type, rp, &len) < 0) { s->closed = 1; return -1; }
        if (type == 23) {                     /* application_data */
            if (len == 0) continue;
            int n = len;
            if (n > max) {
                memcpy(s->app, rp, len);
                s->applen = len; s->apppos = 0;
                n = max;
                memcpy(buf, s->app, n);
                s->apppos = n;
                return n;
            }
            memcpy(buf, rp, n);
            return n;
        } else if (type == 21) {              /* alert (probabil close_notify) */
            s->closed = 1;
            return -1;
        }
        /* type 22 (ex. NewSessionTicket): ignoram, citim mai departe */
    }
}

void tls_close(int h)
{
    (void)h;
    struct tls *s = S;
    if (!s) return;
    if (s->established && !s->closed) {
        uint8_t alert[2] = { 1, 0 };          /* warning, close_notify */
        tls_write_enc(s, 21, alert, 2);
    }
    tcp_cclose(s->tcp);
    s->established = 0;
    s->closed = 1;
    s->used = 0;
}
