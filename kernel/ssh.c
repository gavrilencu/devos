/* Client SSH-2 de la zero pentru DevOS.
 * KEX: curve25519-sha256 (x25519 + SHA-256). Cifru: aes128-ctr.
 * MAC: hmac-sha2-256. Autentificare: parola. Un canal de sesiune + shell.
 * NU verifica cheia gazdei (OS de invatare) — ca la clientul TLS.
 * Ruleaza pe un fir de kernel (poate task_sleep). Debug pe serial. */

#include "ssh.h"
#include "netstack.h"
#include "task.h"
#include "pit.h"
#include "pmm.h"
#include "string.h"
#include "serial.h"
#include "sha256.h"
#include "aes.h"
#include "x25519.h"
#include "ed25519.h"
#include "fs.h"

/* --------- mesaje SSH --------- */
#define MSG_DISCONNECT      1
#define MSG_IGNORE          2
#define MSG_UNIMPLEMENTED   3
#define MSG_DEBUG           4
#define MSG_SERVICE_REQUEST 5
#define MSG_SERVICE_ACCEPT  6
#define MSG_KEXINIT        20
#define MSG_NEWKEYS        21
#define MSG_KEX_ECDH_INIT  30
#define MSG_KEX_ECDH_REPLY 31
#define MSG_USERAUTH_REQUEST 50
#define MSG_USERAUTH_FAILURE 51
#define MSG_USERAUTH_SUCCESS 52
#define MSG_USERAUTH_BANNER  53
#define MSG_USERAUTH_INFO_REQUEST  60   /* keyboard-interactive */
#define MSG_USERAUTH_INFO_RESPONSE 61
#define MSG_GLOBAL_REQUEST   80
#define MSG_CHANNEL_OPEN            90
#define MSG_CHANNEL_OPEN_CONFIRM    91
#define MSG_CHANNEL_OPEN_FAILURE    92
#define MSG_CHANNEL_WINDOW_ADJUST   93
#define MSG_CHANNEL_DATA            94
#define MSG_CHANNEL_EXTENDED_DATA   95
#define MSG_CHANNEL_EOF             96
#define MSG_CHANNEL_CLOSE           97
#define MSG_CHANNEL_REQUEST         98
#define MSG_CHANNEL_SUCCESS         99
#define MSG_CHANNEL_FAILURE        100

#define MAC_LEN 32
#define ACC_CAP 40960
#define OUT_CAP 40960
#define PAY_CAP 40960
#define RX_CAP  32768
#define TX_CAP  4096

/* --------- stare sesiune --------- */
static volatile int  st;           /* 0 idle, 1 in curs, 2 gata, -1 eroare */
static const char   *errmsg = "";
static int   sock = -1;
static uint32_t s_ip;
static uint16_t s_port;
static char  s_user[64];
static char  s_pass[64];

/* cheie privata Ed25519 pentru autentificare cu cheie publica */
static uint8_t ssh_seed[32];       /* seed-ul privat (32 octeti) */
static uint8_t ssh_pub[32];        /* cheia publica derivata */
static int     have_key;           /* 1 daca exista o cheie incarcata */

static uint8_t *acc, *outp, *pay, *macbuf, *rxring, *txbuf;
static int accn;                   /* octeti in acc */
static int in_have_len; static uint32_t cur_plen;
static int rxr_head, rxr_tail;     /* ring de la server -> user */
static int txn;                    /* octeti in txbuf de trimis */
static uint32_t send_seq, recv_seq;
static int enc;                    /* 1 dupa NEWKEYS */

/* cifru AES-CTR pe fiecare directie */
typedef struct { aes_ctx k; uint8_t ctr[16]; uint8_t ks[16]; int kspos; } ctr_state;
static ctr_state tx_ctr, rx_ctr;
static uint8_t mac_c2s[32], mac_s2c[32];

/* KEX */
static uint8_t eph_priv[32], eph_pub[32];
static uint8_t sess_id[32];
static char v_s[256]; static int v_s_len;
static const char *V_C = "SSH-2.0-DevOS_1.0";
static uint8_t *i_c; static int i_c_len;    /* payload KEXINIT trimis */
static uint8_t *i_s; static int i_s_len;    /* payload KEXINIT primit */

/* canal */
static uint32_t local_chan = 0, remote_chan;
static int32_t  server_window;     /* fereastra pt. datele pe care le trimitem noi */
static uint32_t our_window;        /* fereastra pe care o oferim serverului */
#define WIN_INIT   0x100000
#define WIN_LOW    0x20000
#define CHAN_MAXPKT 0x4000

/* --------- PRNG (rdtsc) --------- */
static uint64_t rng;
static uint64_t rdtsc(void){ uint32_t a,d; __asm__ volatile("rdtsc":"=a"(a),"=d"(d)); return ((uint64_t)d<<32)|a; }
static void ssh_rand(uint8_t *b, int n){
    for(int i=0;i<n;i++){
        rng = rng*6364136223846793005ull + 1442695040888963407ull;
        rng ^= rdtsc() + pit_ticks();
        b[i] = (uint8_t)(rng >> 33);
    }
}

static void dbg(const char *s){ serial_write("[ssh] "); serial_write(s); serial_write("\n"); }
static void fail(const char *m){ errmsg = m; st = -1; dbg(m); }

/* --------- helpers octeti big-endian --------- */
static void put32(uint8_t *p, uint32_t v){ p[0]=v>>24; p[1]=v>>16; p[2]=v>>8; p[3]=v; }
static uint32_t get32(const uint8_t *p){ return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3]; }

/* --------- retea (polling cu timeout) --------- */
static int net_send_all(const uint8_t *b, int n){
    int off=0, guard=0;
    while(off<n){
        int w = tcp_csend(sock, b+off, n-off);
        if(w>0){ off+=w; guard=0; }
        else { if(tcp_status(sock)==0) return -1; if(++guard>1000) return -1; task_sleep(2); }
    }
    return 0;
}

/* citeste octetii disponibili in acc; intoarce cati a adaugat (0=nimic) */
static int net_pump_raw(void){
    int total=0, n;
    while(accn < ACC_CAP){
        n = tcp_crecv(sock, acc+accn, ACC_CAP-accn);
        if(n<=0) break;
        accn += n; total += n;
    }
    return total;
}

/* --------- CTR --------- */
static void ctr_init(ctr_state *s, const uint8_t *key, const uint8_t *iv){
    aes_init(&s->k, key, 128);
    memcpy(s->ctr, iv, 16);
    s->kspos = 16;
}
static void ctr_xor(ctr_state *s, uint8_t *d, int n){
    for(int i=0;i<n;i++){
        if(s->kspos==16){
            aes_encrypt(&s->k, s->ctr, s->ks);
            s->kspos=0;
            for(int j=15;j>=0;j--){ if(++s->ctr[j]) break; }
        }
        d[i] ^= s->ks[s->kspos++];
    }
}

/* --------- trimite un pachet (payload = mesajul SSH) --------- */
static int send_packet(const uint8_t *payload, int plen){
    int bs = enc ? 16 : 8;
    int padlen = bs - ((4 + 1 + plen) % bs);
    if(padlen < 4) padlen += bs;
    uint32_t packet_length = 1 + plen + padlen;
    int pktlen = 4 + packet_length;
    if(pktlen + MAC_LEN > OUT_CAP){ fail("pachet prea mare la trimitere"); return -1; }
    put32(outp, packet_length);
    outp[4] = (uint8_t)padlen;
    memcpy(outp+5, payload, plen);
    ssh_rand(outp+5+plen, padlen);
    if(enc){
        uint8_t seqb[4]; put32(seqb, send_seq);
        memcpy(macbuf, seqb, 4);
        memcpy(macbuf+4, outp, pktlen);
        hmac_sha256(mac_c2s, 32, macbuf, 4+pktlen, outp+pktlen);
        ctr_xor(&tx_ctr, outp, pktlen);
        pktlen += MAC_LEN;
    }
    send_seq++;
    return net_send_all(outp, pktlen);
}

/* incearca sa extraga un pachet complet din acc; 1=payload in pay/paylen, 0=inca nimic */
static int paylen;
static int try_packet(void){
    if(!enc){
        if(accn < 5) return 0;
        uint32_t plen = get32(acc);
        uint32_t total = 4 + plen;
        if(total > ACC_CAP){ fail("pachet clar prea mare"); return 0; }
        if((uint32_t)accn < total) return 0;
        int padlen = acc[4];
        paylen = (int)plen - 1 - padlen;
        if(paylen < 0){ fail("padding invalid"); return 0; }
        memcpy(pay, acc+5, paylen);
        memmove(acc, acc+total, accn-total); accn -= total;
        recv_seq++;
        return 1;
    }
    /* criptat */
    if(!in_have_len){
        if(accn < 16) return 0;
        ctr_xor(&rx_ctr, acc, 16);      /* decriptam primul bloc */
        cur_plen = get32(acc);
        if(cur_plen < 12 || cur_plen > ACC_CAP){ fail("lungime pachet invalida"); return 0; }
        in_have_len = 1;
    }
    uint32_t total = 4 + cur_plen + MAC_LEN;
    if((uint32_t)accn < total) return 0;
    ctr_xor(&rx_ctr, acc+16, (4+cur_plen) - 16);  /* restul pachetului */
    /* verificam MAC-ul peste seq || pachet clar */
    uint8_t seqb[4]; put32(seqb, recv_seq);
    memcpy(macbuf, seqb, 4);
    memcpy(macbuf+4, acc, 4+cur_plen);
    uint8_t mac[32];
    hmac_sha256(mac_s2c, 32, macbuf, 4 + 4 + cur_plen, mac);   /* seq(4) + pachet(4+cur_plen) */
    if(memcmp(mac, acc+4+cur_plen, MAC_LEN)!=0){
        in_have_len = 0;
        fail("MAC gresit"); return 0;
    }
    int padlen = acc[4];
    paylen = (int)cur_plen - 1 - padlen;
    if(paylen < 0){ fail("padding invalid"); return 0; }
    memcpy(pay, acc+5, paylen);
    memmove(acc, acc+total, accn-total); accn -= total;
    in_have_len = 0; recv_seq++;
    return 1;
}

/* asteapta un pachet (blocant cu timeout); 1=ok in pay/paylen, 0=timeout/eroare */
static int recv_packet(int timeout_ms){
    int waited=0;
    for(;;){
        if(try_packet()) return 1;
        if(st < 0) return 0;
        net_pump_raw();
        if(try_packet()) return 1;
        if(tcp_status(sock)==0 && accn==0){ fail("conexiune inchisa"); return 0; }
        task_sleep(5); waited+=5;
        if(waited > timeout_ms){ fail("timeout la receptie"); return 0; }
    }
}

/* --------- constructori de campuri --------- */
static int put_string(uint8_t *p, const void *s, int n){ put32(p, n); memcpy(p+4, s, n); return 4+n; }
static int put_cstr(uint8_t *p, const char *s){ int n=(int)strlen(s); return put_string(p, s, n); }

/* mpint: intreg pozitiv, minimal, cu 0x00 in fata daca bitul de sus e setat */
static int put_mpint(uint8_t *p, const uint8_t *v, int n){
    int i=0; while(i<n && v[i]==0) i++;         /* sare zerourile din fata */
    int body = n-i;
    int lead = (body>0 && (v[i]&0x80)) ? 1 : 0;
    put32(p, body+lead);
    int o=4;
    if(lead) p[o++]=0;
    memcpy(p+o, v+i, body); o+=body;
    return o;
}

/* --------- hash de schimb --------- */
static void hash_string(sha256_ctx *c, const void *s, int n){
    uint8_t l[4]; put32(l, n); sha256_update(c, l, 4); sha256_update(c, s, n);
}

/* --------- KEXINIT --------- */
static const char *KEX_ALG   = "curve25519-sha256,curve25519-sha256@libssh.org";
static const char *HKEY_ALG  = "ssh-ed25519,rsa-sha2-256,rsa-sha2-512,ssh-rsa";
static const char *ENC_ALG   = "aes128-ctr";
static const char *MAC_ALG   = "hmac-sha2-256";
static const char *COMP_ALG  = "none";

static int build_kexinit(uint8_t *p){
    int o=0;
    p[o++] = MSG_KEXINIT;
    ssh_rand(p+o, 16); o+=16;            /* cookie */
    o += put_cstr(p+o, KEX_ALG);
    o += put_cstr(p+o, HKEY_ALG);
    o += put_cstr(p+o, ENC_ALG);         /* enc c2s */
    o += put_cstr(p+o, ENC_ALG);         /* enc s2c */
    o += put_cstr(p+o, MAC_ALG);         /* mac c2s */
    o += put_cstr(p+o, MAC_ALG);         /* mac s2c */
    o += put_cstr(p+o, COMP_ALG);        /* comp c2s */
    o += put_cstr(p+o, COMP_ALG);        /* comp s2c */
    o += put_cstr(p+o, "");              /* lang c2s */
    o += put_cstr(p+o, "");              /* lang s2c */
    p[o++] = 0;                          /* first_kex_packet_follows */
    put32(p+o, 0); o+=4;                 /* reserved */
    return o;
}

/* deriva o cheie: HASH(K_mpint || H || X || session_id), primii outlen octeti */
static void derive_key(uint8_t *out, int outlen, char X,
                       const uint8_t *kmp, int kmplen, const uint8_t *H){
    sha256_ctx c; sha256_init(&c);
    sha256_update(&c, kmp, kmplen);
    sha256_update(&c, H, 32);
    sha256_update(&c, &X, 1);
    sha256_update(&c, sess_id, 32);
    uint8_t k1[32]; sha256_final(&c, k1);
    int n = outlen<32?outlen:32;
    memcpy(out, k1, n);
    if(outlen>32){                        /* extindere (nu e nevoie pt. aes128/hmac256) */
        sha256_init(&c); sha256_update(&c, kmp, kmplen); sha256_update(&c, H, 32);
        sha256_update(&c, k1, 32); uint8_t k2[32]; sha256_final(&c, k2);
        memcpy(out+32, k2, outlen-32);
    }
}

/* cauta un nume (ex. "password") intr-o name-list SSH (octeti, virgule) */
static int list_has(const uint8_t *s, int n, const char *name){
    int nl = (int)strlen(name);
    int i = 0;
    while(i < n){
        int j = i;
        while(j < n && s[j] != ',') j++;
        if(j - i == nl){
            int k = 0; while(k < nl && s[i+k] == (uint8_t)name[k]) k++;
            if(k == nl) return 1;
        }
        i = j + 1;
    }
    return 0;
}

/* asteapta rezultatul unei incercari de autentificare.
 * 1 = SUCCESS; 0 = FAILURE (metodele permise in fail_methods/fail_len);
 * 2 = a venit INFO_REQUEST (keyboard-interactive, in pay); -1 = eroare. */
static uint8_t fail_methods[256]; static int fail_len;
static int auth_result(void){
    for(;;){
        if(!recv_packet(15000)) return -1;
        uint8_t t = pay[0];
        if(t == MSG_USERAUTH_SUCCESS) return 1;
        if(t == MSG_USERAUTH_INFO_REQUEST) return 2;
        if(t == MSG_USERAUTH_FAILURE){
            uint32_t l = get32(pay+1);
            if(l > sizeof(fail_methods)) l = sizeof(fail_methods);
            memcpy(fail_methods, pay+5, l); fail_len = (int)l;
            return 0;
        }
        if(t == MSG_USERAUTH_BANNER || t == MSG_DEBUG || t == MSG_IGNORE) continue;
        /* orice altceva: ignoram si asteptam */
    }
}

/* trimite raspunsurile la un SSH_MSG_USERAUTH_INFO_REQUEST (keyboard-interactive):
 * completeaza fiecare prompt cu parola. Intoarce 0 ok, -1 eroare. */
static int kbdint_respond(void){
    /* pay: 60, string name, string instruction, string lang, u32 nprompts, [string prompt, bool echo]* */
    int o = 1;
    o += 4 + (int)get32(pay+o);            /* name */
    o += 4 + (int)get32(pay+o);            /* instruction */
    o += 4 + (int)get32(pay+o);            /* language */
    uint32_t np = get32(pay+o); o += 4;
    if(np > 32) return -1;                  /* servere sanatoase: 1-2 prompturi */
    int plen = (int)strlen(s_pass);
    uint8_t m[512]; int mo = 0;
    m[mo++] = MSG_USERAUTH_INFO_RESPONSE;
    put32(m+mo, np); mo += 4;
    for(uint32_t i = 0; i < np; i++){
        /* raspundem cu parola la fiecare prompt */
        if(mo + 4 + plen > (int)sizeof(m)) return -1;
        mo += put_cstr(m+mo, s_pass);
    }
    return send_packet(m, mo);
}

/* --------- handshake complet --------- */
static int do_handshake(void){
    /* 1. schimb de versiuni */
    char hello[64]; int hl=0;
    for(const char*p=V_C;*p;p++) hello[hl++]=*p;
    hello[hl++]='\r'; hello[hl++]='\n';
    if(net_send_all((uint8_t*)hello, hl)<0){ fail("nu pot trimite versiunea"); return -1; }

    /* citim liniile pana la una care incepe cu "SSH-" */
    v_s_len=0; int waited=0;
    for(;;){
        /* extragem o linie din acc */
        int nl=-1;
        for(int i=0;i<accn;i++){ if(acc[i]=='\n'){ nl=i; break; } }
        if(nl>=0){
            int len=nl; if(len>0 && acc[len-1]=='\r') len--;
            if(len>=4 && acc[0]=='S'&&acc[1]=='S'&&acc[2]=='H'&&acc[3]=='-'){
                if(len>255) len=255;
                memcpy(v_s, acc, len); v_s[len]=0; v_s_len=len;
                memmove(acc, acc+nl+1, accn-(nl+1)); accn-=(nl+1);
                break;
            }
            memmove(acc, acc+nl+1, accn-(nl+1)); accn-=(nl+1);  /* linie de banner: ignora */
            continue;
        }
        net_pump_raw();
        if(tcp_status(sock)==0 && accn==0){ fail("server inchis la versiune"); return -1; }
        task_sleep(5); waited+=5;
        if(waited>8000){ fail("timeout versiune"); return -1; }
    }
    dbg(v_s);

    /* 2. KEXINIT propriu */
    i_c_len = build_kexinit(i_c);
    if(send_packet(i_c, i_c_len)<0) return -1;

    /* 3. asteptam KEXINIT-ul serverului */
    for(;;){
        if(!recv_packet(10000)) return -1;
        if(paylen>=1 && pay[0]==MSG_KEXINIT) break;
        if(paylen>=1 && (pay[0]==MSG_DEBUG||pay[0]==MSG_IGNORE)) continue;
    }
    i_s_len = paylen; memcpy(i_s, pay, paylen);
    dbg("KEXINIT primit");

    /* 4. ECDH init: generam efemerul si trimitem Q_C */
    ssh_rand(eph_priv, 32);
    x25519_base(eph_pub, eph_priv);
    { uint8_t m[64]; int o=0; m[o++]=MSG_KEX_ECDH_INIT; o+=put_string(m+o, eph_pub, 32);
      if(send_packet(m,o)<0) return -1; }

    /* 5. KEX_ECDH_REPLY: K_S, Q_S, semnatura */
    for(;;){
        if(!recv_packet(10000)) return -1;
        if(paylen>=1 && pay[0]==MSG_KEX_ECDH_REPLY) break;
        if(paylen>=1 && (pay[0]==MSG_DEBUG||pay[0]==MSG_IGNORE)) continue;
    }
    {
        int o=1;
        uint32_t ksl = get32(pay+o); o+=4;
        uint8_t *K_S = pay+o; int K_S_len = ksl; o+=ksl;
        uint32_t qsl = get32(pay+o); o+=4;
        if(qsl!=32){ fail("Q_S nu are 32 octeti"); return -1; }
        uint8_t Q_S[32]; memcpy(Q_S, pay+o, 32); o+=32;
        /* semnatura (pay+o..) — NU o verificam */

        uint8_t K[32]; x25519(K, eph_priv, Q_S);

        /* hash de schimb H */
        sha256_ctx hc; sha256_init(&hc);
        hash_string(&hc, V_C, (int)strlen(V_C));
        hash_string(&hc, v_s, v_s_len);
        hash_string(&hc, i_c, i_c_len);
        hash_string(&hc, i_s, i_s_len);
        hash_string(&hc, K_S, K_S_len);
        hash_string(&hc, eph_pub, 32);
        hash_string(&hc, Q_S, 32);
        uint8_t kmp[36]; int kmpl = put_mpint(kmp, K, 32);
        sha256_update(&hc, kmp, kmpl);
        uint8_t H[32]; sha256_final(&hc, H);
        memcpy(sess_id, H, 32);           /* prima sesiune: session_id = H */

        /* derivare chei: A=IV c2s, B=IV s2c, C=key c2s, D=key s2c, E=mac c2s, F=mac s2c */
        uint8_t iv_c[16], iv_s[16], key_c[16], key_s[16];
        derive_key(iv_c, 16, 'A', kmp, kmpl, H);
        derive_key(iv_s, 16, 'B', kmp, kmpl, H);
        derive_key(key_c,16, 'C', kmp, kmpl, H);
        derive_key(key_s,16, 'D', kmp, kmpl, H);
        derive_key(mac_c2s,32,'E', kmp, kmpl, H);
        derive_key(mac_s2c,32,'F', kmp, kmpl, H);
        ctr_init(&tx_ctr, key_c, iv_c);
        ctr_init(&rx_ctr, key_s, iv_s);
    }
    dbg("ECDH gata, chei derivate");

    /* 6. NEWKEYS */
    { uint8_t m[1]={MSG_NEWKEYS}; if(send_packet(m,1)<0) return -1; }
    for(;;){
        if(!recv_packet(10000)) return -1;
        if(paylen>=1 && pay[0]==MSG_NEWKEYS) break;
    }
    enc = 1;                              /* de acum tot e criptat */
    dbg("NEWKEYS — canal criptat");

    /* 7. service request ssh-userauth */
    { uint8_t m[64]; int o=0; m[o++]=MSG_SERVICE_REQUEST; o+=put_cstr(m+o,"ssh-userauth");
      if(send_packet(m,o)<0) return -1; }
    for(;;){ if(!recv_packet(10000)) return -1;
             if(paylen>=1 && pay[0]==MSG_SERVICE_ACCEPT) break;
             if(paylen>=1 && (pay[0]==MSG_DEBUG||pay[0]==MSG_IGNORE)) continue; }

    /* 8. userauth. Intai metoda "none" ca sa aflam ce accepta serverul. */
    {
        uint8_t m[256]; int o=0;
        m[o++]=MSG_USERAUTH_REQUEST;
        o+=put_cstr(m+o, s_user);
        o+=put_cstr(m+o, "ssh-connection");
        o+=put_cstr(m+o, "none");
        if(send_packet(m,o)<0) return -1;
    }
    {
        int r = auth_result();
        if(r < 0){ fail("eroare la autentificare (none)"); return -1; }
        if(r == 1){ dbg("autentificat (fara parola)"); goto auth_ok; }
        /* r==0: FAILURE — fail_methods contine metodele permise */
    }

    /* 8p. autentificare cu CHEIE PUBLICA (ed25519), daca avem cheie si serverul
     *     o accepta. Se incearca inaintea parolei (ca la clientul OpenSSH). */
    if(have_key && list_has(fail_methods, fail_len, "publickey")){
        dbg("incerc cheie publica (ed25519)");
        /* blob-ul cheii publice: string "ssh-ed25519" || string pub(32) */
        uint8_t blob[64]; int bl=0;
        bl += put_cstr(blob+bl, "ssh-ed25519");
        bl += put_string(blob+bl, ssh_pub, 32);

        /* datele semnate (RFC 4252 §7): string session_id || restul cererii */
        uint8_t sd[400]; int sl=0;
        sl += put_string(sd+sl, sess_id, 32);
        sd[sl++] = MSG_USERAUTH_REQUEST;
        sl += put_cstr(sd+sl, s_user);
        sl += put_cstr(sd+sl, "ssh-connection");
        sl += put_cstr(sd+sl, "publickey");
        sd[sl++] = 1;                          /* TRUE: cu semnatura */
        sl += put_cstr(sd+sl, "ssh-ed25519");
        sl += put_string(sd+sl, blob, bl);

        uint8_t sig[64];
        ed25519_sign(sig, sd, (unsigned long long)sl, ssh_seed, ssh_pub);

        /* semnatura codata: string "ssh-ed25519" || string sig(64) */
        uint8_t sigb[128]; int sgl=0;
        sgl += put_cstr(sigb+sgl, "ssh-ed25519");
        sgl += put_string(sigb+sgl, sig, 64);

        /* cererea de autentificare propriu-zisa */
        uint8_t m[400]; int o=0;
        m[o++] = MSG_USERAUTH_REQUEST;
        o += put_cstr(m+o, s_user);
        o += put_cstr(m+o, "ssh-connection");
        o += put_cstr(m+o, "publickey");
        m[o++] = 1;                            /* TRUE: cu semnatura */
        o += put_cstr(m+o, "ssh-ed25519");
        o += put_string(m+o, blob, bl);
        o += put_string(m+o, sigb, sgl);
        if(send_packet(m,o)<0) return -1;

        int r = auth_result();
        if(r < 0){ fail("eroare la autentificare (cheie)"); return -1; }
        if(r == 1){ dbg("autentificat (cheie publica)"); goto auth_ok; }
        dbg("cheia publica respinsa (nu e in authorized_keys?)");
        /* r==0: cheia respinsa; incercam parola daca exista */
    }

    /* 8a. autentificare cu parola, daca serverul o ofera */
    if(list_has(fail_methods, fail_len, "password")){
        uint8_t m[512]; int o=0;
        m[o++]=MSG_USERAUTH_REQUEST;
        o+=put_cstr(m+o, s_user);
        o+=put_cstr(m+o, "ssh-connection");
        o+=put_cstr(m+o, "password");
        m[o++]=0;                         /* boolean FALSE */
        o+=put_cstr(m+o, s_pass);
        if(send_packet(m,o)<0) return -1;
        int r = auth_result();
        if(r < 0){ fail("eroare la autentificare (password)"); return -1; }
        if(r == 1){ dbg("autentificat (parola)"); goto auth_ok; }
        /* r==0: parola respinsa; incercam keyboard-interactive daca exista */
    }

    /* 8b. keyboard-interactive — multe servere reale (PAM) o folosesc */
    if(list_has(fail_methods, fail_len, "keyboard-interactive")){
        dbg("incerc keyboard-interactive");
        uint8_t m[256]; int o=0;
        m[o++]=MSG_USERAUTH_REQUEST;
        o+=put_cstr(m+o, s_user);
        o+=put_cstr(m+o, "ssh-connection");
        o+=put_cstr(m+o, "keyboard-interactive");
        o+=put_cstr(m+o, "");            /* language tag (gol) */
        o+=put_cstr(m+o, "");            /* submethods (gol) */
        if(send_packet(m,o)<0) return -1;
        for(;;){
            int r = auth_result();
            if(r < 0){ fail("eroare la autentificare (kbd-interactive)"); return -1; }
            if(r == 1){ dbg("autentificat (kbd-interactive)"); goto auth_ok; }
            if(r == 2){ dbg("prompt kbd-interactive, raspund"); if(kbdint_respond()<0){ fail("kbd-interactive invalid"); return -1; } continue; }
            break;                        /* r==0: FAILURE */
        }
    }

    /* toate metodele au esuat: raportam ce accepta serverul */
    {
        static char em[320]; int p=0;
        for(const char*s="autentificare esuata; server accepta: ";*s && p<(int)sizeof(em)-1;s++) em[p++]=*s;
        for(int i=0;i<fail_len && p<(int)sizeof(em)-1;i++) em[p++]=(char)fail_methods[i];
        em[p]=0;
        fail(em);
        return -1;
    }
auth_ok:;

    /* 9. deschidem canalul de sesiune */
    {
        uint8_t m[64]; int o=0;
        m[o++]=MSG_CHANNEL_OPEN;
        o+=put_cstr(m+o,"session");
        put32(m+o, local_chan); o+=4;
        put32(m+o, WIN_INIT); o+=4;
        put32(m+o, CHAN_MAXPKT); o+=4;
        if(send_packet(m,o)<0) return -1;
    }
    for(;;){
        if(!recv_packet(10000)) return -1;
        if(pay[0]==MSG_CHANNEL_OPEN_CONFIRM){
            remote_chan = get32(pay+5);
            server_window = (int32_t)get32(pay+9);
            our_window = WIN_INIT;
            break;
        }
        if(pay[0]==MSG_CHANNEL_OPEN_FAILURE){ fail("canal refuzat"); return -1; }
        if(pay[0]==MSG_GLOBAL_REQUEST||pay[0]==MSG_DEBUG||pay[0]==MSG_IGNORE) continue;
    }
    dbg("canal deschis");

    /* 10. pty-req + shell */
    {
        uint8_t m[128]; int o=0;
        m[o++]=MSG_CHANNEL_REQUEST;
        put32(m+o, remote_chan); o+=4;
        o+=put_cstr(m+o,"pty-req");
        m[o++]=0;                         /* want_reply = FALSE */
        o+=put_cstr(m+o,"xterm");
        put32(m+o,80); o+=4;              /* coloane */
        put32(m+o,25); o+=4;              /* randuri (consola DevOS e 80x25) */
        put32(m+o,0); o+=4; put32(m+o,0); o+=4;
        o+=put_cstr(m+o,"");              /* modes (gol) */
        if(send_packet(m,o)<0) return -1;
    }
    {
        uint8_t m[32]; int o=0;
        m[o++]=MSG_CHANNEL_REQUEST;
        put32(m+o, remote_chan); o+=4;
        o+=put_cstr(m+o,"shell");
        m[o++]=1;                         /* want_reply = TRUE */
        if(send_packet(m,o)<0) return -1;
    }
    dbg("shell cerut");
    return 0;
}

/* --------- ring RX (server -> user) --------- */
static void rx_push(const uint8_t *d, int n){
    for(int i=0;i<n;i++){
        int nt = (rxr_tail+1)%RX_CAP;
        if(nt==rxr_head) break;           /* plin: aruncam restul */
        rxring[rxr_tail]=d[i]; rxr_tail=nt;
    }
}
static int rx_pop(uint8_t *d, int max){
    int n=0;
    while(n<max && rxr_head!=rxr_tail){ d[n++]=rxring[rxr_head]; rxr_head=(rxr_head+1)%RX_CAP; }
    return n;
}

/* trimite date pe canal (respectand fereastra serverului) */
static uint8_t *cdbuf;                    /* scratch pt. CHANNEL_DATA (pmm) */
static void channel_send(const uint8_t *d, int n){
    while(n>0){
        int chunk = n;
        if(chunk > CHAN_MAXPKT-64) chunk = CHAN_MAXPKT-64;
        if(chunk > server_window) chunk = server_window;
        if(chunk<=0) return;              /* fara fereastra: renuntam (shell, rar) */
        int o=0; cdbuf[o++]=MSG_CHANNEL_DATA; put32(cdbuf+o, remote_chan); o+=4;
        put32(cdbuf+o, chunk); o+=4; memcpy(cdbuf+o, d, chunk); o+=chunk;
        if(send_packet(cdbuf,o)<0) return;
        server_window -= chunk;
        d+=chunk; n-=chunk;
    }
}

static void adjust_our_window(void){
    if(our_window < WIN_LOW){
        uint8_t m[16]; int o=0; m[o++]=MSG_CHANNEL_WINDOW_ADJUST;
        put32(m+o, remote_chan); o+=4; put32(m+o, WIN_INIT); o+=4;
        send_packet(m,o);
        our_window += WIN_INIT;
    }
}

/* proceseaza un pachet primit in faza interactiva */
static void handle_packet(void){
    uint8_t t = pay[0];
    if(t==MSG_CHANNEL_DATA){
        uint32_t dl = get32(pay+5);
        rx_push(pay+9, dl);
        if(our_window > dl) our_window -= dl; else our_window = 0;
        adjust_our_window();
    } else if(t==MSG_CHANNEL_EXTENDED_DATA){
        uint32_t dl = get32(pay+9);       /* skip data_type_code */
        rx_push(pay+13, dl);
        if(our_window > dl) our_window -= dl; else our_window = 0;
        adjust_our_window();
    } else if(t==MSG_CHANNEL_WINDOW_ADJUST){
        server_window += (int32_t)get32(pay+5);
    } else if(t==MSG_CHANNEL_EOF){
        /* ignoram */
    } else if(t==MSG_CHANNEL_CLOSE || t==MSG_DISCONNECT){
        st = 0;                            /* sesiune terminata */
    } else if(t==MSG_CHANNEL_REQUEST){
        /* ex. exit-status: ignoram (want_reply de obicei FALSE) */
    } else if(t==MSG_GLOBAL_REQUEST){
        /* daca want_reply, ar trebui REQUEST_FAILURE; multe servere nu cer */
    }
    /* CHANNEL_SUCCESS/FAILURE la pty/shell: ignorate */
}

/* --------- firul de kernel al sesiunii --------- */
static void ssh_thread(void){
    task_sleep(30);
    sock = tcp_connect(s_ip, s_port);
    if(sock<0){ fail("tcp_connect a esuat"); return; }
    int g=0;
    while(tcp_status(sock)==1){ if(++g>500){ fail("timeout la conectare TCP"); tcp_cclose(sock); return; } task_sleep(20); }
    if(tcp_status(sock)!=2){ fail("nu s-a putut conecta"); tcp_cclose(sock); return; }
    dbg("TCP conectat, incep handshake");

    if(do_handshake()!=0){ tcp_cclose(sock); return; }
    st = 2;                                /* gata: shell interactiv */

    /* bucla interactiva */
    while(st==2){
        int did=0;
        net_pump_raw();
        while(try_packet()){ handle_packet(); did=1; if(st!=2) break; }
        if(st!=2) break;
        if(txn>0){ channel_send(txbuf, txn); txn=0; did=1; }
        if(tcp_status(sock)==0 && accn==0){ st=0; break; }
        if(!did) task_sleep(8);
    }
    tcp_cclose(sock);
    if(st==2) st=0;
    dbg("sesiune inchisa");
}

/* --------- API public --------- */
static int alloc_buffers(void){
    if(acc) return 0;
    uint64_t need = ACC_CAP + OUT_CAP + PAY_CAP + (ACC_CAP+16) + RX_CAP + TX_CAP
                    + 4096 + 4096 + CHAN_MAXPKT + 64;
    uint64_t p = pmm_alloc_contig((need + PMM_FRAME_SIZE-1)/PMM_FRAME_SIZE);
    if(!p) return -1;
    uint8_t *b=(uint8_t*)p;
    acc=b;      b+=ACC_CAP;
    outp=b;     b+=OUT_CAP;
    pay=b;      b+=PAY_CAP;
    macbuf=b;   b+=ACC_CAP+16;
    rxring=b;   b+=RX_CAP;
    txbuf=b;    b+=TX_CAP;
    i_c=b;      b+=4096;
    i_s=b;      b+=4096;
    cdbuf=b;    b+=CHAN_MAXPKT+64;
    return 0;
}

/* incarca cheia privata din fs (fisierul "id_ed25519", 32 octeti seed) */
static void load_ssh_key(void){
    have_key = 0;
    uint8_t s[512];                    /* fs_read_into citeste sectoare intregi */
    if(fs_read_into("id_ed25519", s, sizeof(s)) == 32){
        memcpy(ssh_seed, s, 32);
        ed25519_pubkey(ssh_pub, ssh_seed);
        have_key = 1;
    }
}

int ssh_open(uint32_t ip, uint16_t port, const char *user, const char *pass){
    if(st==1 || st==2) return -1;
    if(alloc_buffers()<0){ errmsg="memorie insuficienta"; return -1; }
    s_ip=ip; s_port=port;
    int i;
    for(i=0;i<63 && user[i];i++) s_user[i]=user[i];
    s_user[i]=0;
    for(i=0;i<63 && pass[i];i++) s_pass[i]=pass[i];
    s_pass[i]=0;
    accn=0; in_have_len=0; rxr_head=rxr_tail=0; txn=0;
    send_seq=recv_seq=0; enc=0; errmsg="";
    rng = rdtsc() ^ (pit_ticks()*0x9e3779b1u);
    load_ssh_key();
    st=1;
    task_create("ssh", ssh_thread);
    return 0;
}

/* --------- gestiune cheie (folosite de programul sshkey) --------- */
static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static int base64_encode(const uint8_t *in, int n, char *out){
    int o=0;
    for(int i=0;i<n;i+=3){
        int b0=in[i], b1=(i+1<n)?in[i+1]:0, b2=(i+2<n)?in[i+2]:0;
        out[o++]=B64[b0>>2];
        out[o++]=B64[((b0&3)<<4)|(b1>>4)];
        out[o++]=(i+1<n)?B64[((b1&15)<<2)|(b2>>6)]:'=';
        out[o++]=(i+2<n)?B64[b2&63]:'=';
    }
    out[o]=0;
    return o;
}

/* genereaza o pereche noua de chei si o salveaza pe disc. 0 ok, -1 eroare. */
int ssh_keygen(void){
    rng = rdtsc() ^ (pit_ticks()*0x9e3779b1u);
    ssh_rand(ssh_seed, 32);
    ed25519_pubkey(ssh_pub, ssh_seed);
    have_key = 1;
    if(fs_save("id_ed25519", ssh_seed, 32) != 0) return -1;
    { char line[256]; if(ssh_get_pubkey_line(line, sizeof(line))>0){ dbg("cheie publica:"); dbg(line); } }
    return 0;
}

/* scrie linia pentru authorized_keys ("ssh-ed25519 <base64> devos@devos").
 * Intoarce lungimea sau -1 daca nu exista cheie. */
int ssh_get_pubkey_line(char *buf, int max){
    if(!have_key) load_ssh_key();
    if(!have_key) return -1;
    uint8_t blob[64]; int bl=0;
    bl += put_cstr(blob+bl, "ssh-ed25519");
    bl += put_string(blob+bl, ssh_pub, 32);
    char b64[128]; int bn = base64_encode(blob, bl, b64);
    int o=0;
    const char *pre="ssh-ed25519 ";
    for(const char*p=pre;*p&&o<max-1;p++) buf[o++]=*p;
    for(int i=0;i<bn&&o<max-1;i++) buf[o++]=b64[i];
    const char *suf=" devos@devos";
    for(const char*p=suf;*p&&o<max-1;p++) buf[o++]=*p;
    buf[o]=0;
    return o;
}

int ssh_status(void){ return st; }

int ssh_read(void *buf, int max){
    if(!rxring) return 0;
    return rx_pop((uint8_t*)buf, max);
}

int ssh_write(const void *buf, int len){
    if(st!=2) return 0;
    const uint8_t *b=buf; int w=0;
    while(w<len && txn<TX_CAP){ txbuf[txn++]=b[w++]; }
    return w;
}

void ssh_close(void){
    if(st==2||st==1) st=0;
}

const char *ssh_error(void){ return errmsg; }
