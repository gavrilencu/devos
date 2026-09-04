/* Decompresor DEFLATE (RFC 1951). Compact, decodare Huffman bit-cu-bit. */
#include "inflate.h"

struct bs {
    const uint8_t *d; int len; int pos; uint32_t bitbuf; int bitcnt;
    int err;
};

static int getbit(struct bs *b)
{
    if (b->bitcnt == 0) {
        if (b->pos >= b->len) { b->err = 1; return 0; }
        b->bitbuf = b->d[b->pos++];
        b->bitcnt = 8;
    }
    int bit = b->bitbuf & 1;
    b->bitbuf >>= 1;
    b->bitcnt--;
    return bit;
}
static int getbits(struct bs *b, int n)
{
    int v = 0;
    for (int i = 0; i < n; i++) v |= getbit(b) << i;
    return v;
}

/* Huffman: coduri canonice date de lungimi. Decodare bit-cu-bit. */
struct huff {
    uint16_t count[16];       /* cate coduri de fiecare lungime */
    uint16_t sym[288];        /* simbolurile sortate dupa (lungime, valoare) */
};

static void huff_build(struct huff *h, const uint8_t *lengths, int n)
{
    for (int i = 0; i < 16; i++) h->count[i] = 0;
    for (int i = 0; i < n; i++) h->count[lengths[i]]++;
    h->count[0] = 0;
    uint16_t offs[16]; offs[0] = 0; offs[1] = 0;
    for (int i = 1; i < 15; i++) offs[i + 1] = offs[i] + h->count[i];
    for (int i = 0; i < n; i++)
        if (lengths[i]) h->sym[offs[lengths[i]]++] = (uint16_t)i;
}

static int huff_decode(struct bs *b, struct huff *h)
{
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= 15; len++) {
        code |= getbit(b);
        int cnt = h->count[len];
        if (code - first < cnt) return h->sym[index + (code - first)];
        index += cnt;
        first += cnt;
        first <<= 1;
        code <<= 1;
        if (b->err) return -1;
    }
    return -1;
}

static const uint16_t len_base[29] = {
    3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258 };
static const uint8_t len_extra[29] = {
    0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0 };
static const uint16_t dist_base[30] = {
    1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577 };
static const uint8_t dist_extra[30] = {
    0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13 };

static int inflate_block(struct bs *b, struct huff *lit, struct huff *dist,
                         uint8_t *out, int outcap, int outpos)
{
    for (;;) {
        int sym = huff_decode(b, lit);
        if (sym < 0 || b->err) return -1;
        if (sym == 256) return outpos;            /* sfarsit bloc */
        if (sym < 256) {
            if (outpos >= outcap) return -1;
            out[outpos++] = (uint8_t)sym;
        } else {
            sym -= 257;
            if (sym >= 29) return -1;
            int length = len_base[sym] + getbits(b, len_extra[sym]);
            int ds = huff_decode(b, dist);
            if (ds < 0 || ds >= 30) return -1;
            int distance = dist_base[ds] + getbits(b, dist_extra[ds]);
            if (distance > outpos) return -1;
            for (int i = 0; i < length; i++) {
                if (outpos >= outcap) return -1;
                out[outpos] = out[outpos - distance];
                outpos++;
            }
        }
        if (b->err) return -1;
    }
}

int inflate(const uint8_t *in, int inlen, uint8_t *out, int outcap)
{
    struct bs b = { in, inlen, 0, 0, 0, 0 };
    int outpos = 0;
    int final;
    do {
        final = getbit(&b);
        int type = getbits(&b, 2);
        if (b.err) return -1;
        if (type == 0) {                          /* bloc necomprimat */
            b.bitbuf = 0; b.bitcnt = 0;           /* aliniere la octet */
            if (b.pos + 4 > b.len) return -1;
            int len = b.d[b.pos] | (b.d[b.pos+1] << 8);
            b.pos += 4;                            /* sarim LEN + NLEN */
            if (b.pos + len > b.len || outpos + len > outcap) return -1;
            for (int i = 0; i < len; i++) out[outpos++] = b.d[b.pos++];
        } else if (type == 1) {                   /* Huffman fix */
            static struct huff lit, dist; static int built = 0;
            if (!built) {
                uint8_t ll[288];
                for (int i = 0; i < 144; i++) ll[i] = 8;
                for (int i = 144; i < 256; i++) ll[i] = 9;
                for (int i = 256; i < 280; i++) ll[i] = 7;
                for (int i = 280; i < 288; i++) ll[i] = 8;
                huff_build(&lit, ll, 288);
                uint8_t dl[30]; for (int i = 0; i < 30; i++) dl[i] = 5;
                huff_build(&dist, dl, 30);
                built = 1;
            }
            outpos = inflate_block(&b, &lit, &dist, out, outcap, outpos);
            if (outpos < 0) return -1;
        } else if (type == 2) {                   /* Huffman dinamic */
            int hlit = getbits(&b, 5) + 257;
            int hdist = getbits(&b, 5) + 1;
            int hclen = getbits(&b, 4) + 4;
            static const uint8_t ord[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
            uint8_t cl[19]; for (int i = 0; i < 19; i++) cl[i] = 0;
            for (int i = 0; i < hclen; i++) cl[ord[i]] = (uint8_t)getbits(&b, 3);
            struct huff clh;
            huff_build(&clh, cl, 19);
            uint8_t lengths[288 + 32];
            int n = 0, total = hlit + hdist;
            while (n < total) {
                int s = huff_decode(&b, &clh);
                if (s < 0) return -1;
                if (s < 16) lengths[n++] = (uint8_t)s;
                else if (s == 16) { if (n == 0) return -1; int r = getbits(&b,2)+3; uint8_t p=lengths[n-1]; while(r--&&n<total)lengths[n++]=p; }
                else if (s == 17) { int r = getbits(&b,3)+3; while(r--&&n<total)lengths[n++]=0; }
                else { int r = getbits(&b,7)+11; while(r--&&n<total)lengths[n++]=0; }
                if (b.err) return -1;
            }
            struct huff lit, dist;
            huff_build(&lit, lengths, hlit);
            huff_build(&dist, lengths + hlit, hdist);
            outpos = inflate_block(&b, &lit, &dist, out, outcap, outpos);
            if (outpos < 0) return -1;
        } else return -1;
    } while (!final && !b.err);
    return b.err ? -1 : outpos;
}
