/* Decodor PNG minimal (8 biti/canal, fara interlace). */
#include "png.h"
#include "inflate.h"
#include "string.h"

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3]; }

static int paeth(int a, int b, int c)
{
    int p = a + b - c;
    int pa = p > a ? p - a : a - p;
    int pb = p > b ? p - b : b - p;
    int pc = p > c ? p - c : c - p;
    if (pa <= pb && pa <= pc) return a;
    if (pb <= pc) return b;
    return c;
}

int png_decode(const uint8_t *in, int inlen, uint32_t *pix, int maxpix,
               int *w, int *h, uint8_t *scratch, int scratchcap)
{
    static const uint8_t sig[8] = { 137,80,78,71,13,10,26,10 };
    if (inlen < 8 || memcmp(in, sig, 8) != 0) return -1;

    int width = 0, height = 0, bitdepth = 0, color = 0, interlace = 0;
    uint8_t palette[256*3]; int have_plte = 0;
    /* colectam IDAT intr-un buffer temporar (folosim jumatatea a doua a scratch) */
    int idat_cap = scratchcap / 2;
    uint8_t *idat = scratch + idat_cap;
    int idat_len = 0;

    int p = 8;
    while (p + 8 <= inlen) {
        uint32_t clen = rd32(in + p);
        const uint8_t *type = in + p + 4;
        const uint8_t *data = in + p + 8;
        if (p + 12 + (int)clen > inlen) break;
        if (memcmp(type, "IHDR", 4) == 0) {
            width = (int)rd32(data);
            height = (int)rd32(data + 4);
            bitdepth = data[8]; color = data[9]; interlace = data[12];
        } else if (memcmp(type, "PLTE", 4) == 0) {
            int n = (int)clen; if (n > 256*3) n = 256*3;
            memcpy(palette, data, n); have_plte = 1;
        } else if (memcmp(type, "IDAT", 4) == 0) {
            if (idat_len + (int)clen <= idat_cap) { memcpy(idat + idat_len, data, clen); idat_len += clen; }
        } else if (memcmp(type, "IEND", 4) == 0) {
            break;
        }
        p += 12 + clen;
    }
    if (width <= 0 || height <= 0 || bitdepth != 8 || interlace != 0) return -1;
    if (idat_len < 3) return -1;

    int channels;
    switch (color) {
        case 0: channels = 1; break;   /* gri */
        case 2: channels = 3; break;   /* RGB */
        case 3: channels = 1; break;   /* paleta (index) */
        case 4: channels = 2; break;   /* gri + alfa */
        case 6: channels = 4; break;   /* RGBA */
        default: return -1;
    }
    if (color == 3 && !have_plte) return -1;

    int stride = width * channels;
    int raw_need = height * (stride + 1);
    if (raw_need > idat_cap) return -1;

    /* zlib: sarim antetul de 2 octeti (CMF+FLG), inflate restul (fara adler) */
    int n = inflate(idat + 2, idat_len - 2, scratch, idat_cap);
    if (n < raw_need) return -1;

    /* unfilter, pe loc: producem `stride` octeti per rand in `line` continuu */
    uint8_t *raw = scratch;               /* [filter][scanline] per rand */
    uint8_t *cur, *prev = 0;
    /* compactam scanline-urile eliminand octetul de filtru */
    static uint8_t rows_static;           /* (nefolosit) */
    (void)rows_static;
    for (int y = 0; y < height; y++) {
        uint8_t filt = raw[y * (stride + 1)];
        cur = raw + y * (stride + 1) + 1;
        for (int x = 0; x < stride; x++) {
            int a = x >= channels ? cur[x - channels] : 0;
            int b = prev ? prev[x] : 0;
            int c = (prev && x >= channels) ? prev[x - channels] : 0;
            int v = cur[x];
            switch (filt) {
                case 0: break;
                case 1: v = (v + a) & 0xff; break;
                case 2: v = (v + b) & 0xff; break;
                case 3: v = (v + ((a + b) >> 1)) & 0xff; break;
                case 4: v = (v + paeth(a, b, c)) & 0xff; break;
                default: return -1;
            }
            cur[x] = (uint8_t)v;
        }
        prev = cur;
    }

    if ((long)width * height > maxpix) return -1;

    /* conversie la 0x00RRGGBB (alfa compus peste alb) */
    for (int y = 0; y < height; y++) {
        uint8_t *row = raw + y * (stride + 1) + 1;
        for (int x = 0; x < width; x++) {
            int r, g, bl, al = 255;
            if (color == 2) { r = row[x*3]; g = row[x*3+1]; bl = row[x*3+2]; }
            else if (color == 6) { r = row[x*4]; g = row[x*4+1]; bl = row[x*4+2]; al = row[x*4+3]; }
            else if (color == 0) { r = g = bl = row[x]; }
            else if (color == 4) { r = g = bl = row[x*2]; al = row[x*2+1]; }
            else { int idx = row[x]; r = palette[idx*3]; g = palette[idx*3+1]; bl = palette[idx*3+2]; }
            if (al != 255) {
                r = (r * al + 255 * (255 - al)) / 255;
                g = (g * al + 255 * (255 - al)) / 255;
                bl = (bl * al + 255 * (255 - al)) / 255;
            }
            pix[y * width + x] = ((uint32_t)r << 16) | ((uint32_t)g << 8) | bl;
        }
    }
    *w = width; *h = height;
    return 0;
}
