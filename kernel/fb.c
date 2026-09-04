#include "fb.h"
#include "vmm.h"
#include "pmm.h"
#include "io.h"

/* Completat de stage2 (vezi boot/stage2.asm, VBE_SAVE). */
#define VBE_SAVE 0x5F00
struct vbe_save {
    uint16_t w, h, pitch;
    uint8_t bpp, active;
    uint32_t fb;
} __attribute__((packed));

/* Fontul 8x16 al BIOS-ului, copiat de stage2 la 0x6000. */
static const uint8_t *font = (const uint8_t *)0x6000;

static volatile uint32_t *fbp;   /* framebuffer-ul real (vizibil pe ecran) */
static uint32_t *back;           /* back buffer: desenam aici, apoi copiem */
static uint32_t *draw;           /* tinta desenelor (= back daca exista) */
static int W, H, pitch32;
static int maxW, maxH;           /* rezolutia de boot = maximul suportat (LFB + back buffer) */
static int dstride;              /* stride-ul lui `draw` (W sau pitch32) */
static int active;
static int cx0, cy0, cx1, cy1;   /* clipping (cx1/cy1 = exclusiv) */

/* dreptunghiul "murdar" (modificat de la ultima prezentare pe ecran) */
static int dx0, dy0, dx1, dy1, dirty;

static uint32_t blend(uint32_t fg, uint32_t bg, int a);   /* definit mai jos */

static void mark(int x, int y, int w, int h)
{
    if (!back)
        return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > W) w = W - x;
    if (y + h > H) h = H - y;
    if (w <= 0 || h <= 0)
        return;
    if (!dirty) {
        dx0 = x; dy0 = y; dx1 = x + w; dy1 = y + h;
        dirty = 1;
    } else {
        if (x < dx0) dx0 = x;
        if (y < dy0) dy0 = y;
        if (x + w > dx1) dx1 = x + w;
        if (y + h > dy1) dy1 = y + h;
    }
}

/* copiaza pe ecran regiunea desenata de la ultima prezentare. Chemat des
 * (din IRQ-ul de timer) — o singura copie rapida, deci fara tearing. */
void fb_flush(void)
{
    if (!back || !dirty)
        return;
    int x0 = dx0, y0 = dy0, x1 = dx1, y1 = dy1;
    dirty = 0;
    for (int y = y0; y < y1; y++) {
        const uint32_t *s = back + (uint64_t)y * W + x0;
        volatile uint32_t *d = fbp + (uint64_t)y * pitch32 + x0;
        for (int x = x0; x < x1; x++)
            d[x - x0] = s[x - x0];
    }
}

int fb_init(void)
{
    const volatile struct vbe_save *v = (const volatile struct vbe_save *)VBE_SAVE;
    if (v->active != 1 || v->bpp != 32)
        return 0;

    W = v->w;
    H = v->h;
    maxW = W;
    maxH = H;
    pitch32 = v->pitch / 4;

    /* Framebuffer-ul e in spatiul PCI (~4 GiB), in afara identity map-ului
     * de RAM — il mapam explicit. Fiind in PML4[0], e vizibil (doar pentru
     * kernel) din toate spatiile de adrese. */
    uint64_t base = v->fb;
    uint64_t size = (uint64_t)v->pitch * H;
    for (uint64_t off = 0; off < size; off += PMM_FRAME_SIZE)
        if (vmm_map(base + off, base + off, VMM_W) < 0)
            return 0;

    fbp = (volatile uint32_t *)base;

    /* back buffer in RAM (W*H*4). Desenam aici si copiem pe ecran cu
     * fb_flush — asa QEMU nu prinde niciodata un desen la jumatate. */
    uint64_t bb_bytes = (uint64_t)W * H * 4;
    uint64_t bb = pmm_alloc_contig((bb_bytes + PMM_FRAME_SIZE - 1) / PMM_FRAME_SIZE);
    back = bb ? (uint32_t *)bb : 0;
    draw = back ? back : (uint32_t *)fbp;
    dstride = back ? W : pitch32;

    cx0 = 0;
    cy0 = 0;
    cx1 = W;
    cy1 = H;
    active = 1;
    return 1;
}

/* Interfata Bochs VBE dispi (QEMU/Bochs stdvga) — schimbare de rezolutie
 * la cald. Adresa LFB si back buffer-ul raman cele de la boot (dimensionate
 * pentru rezolutia maxima), deci acceptam doar rezolutii <= max. */
#define DISPI_IDX 0x01CE
#define DISPI_DAT 0x01CF

static void dispi_write(uint16_t idx, uint16_t val)
{
    outw(DISPI_IDX, idx);
    outw(DISPI_DAT, val);
}

static uint16_t dispi_read(uint16_t idx)
{
    outw(DISPI_IDX, idx);
    return inw(DISPI_DAT);
}

int fb_set_mode(int w, int h)
{
    if (!active || w <= 0 || h <= 0 || w > maxW || h > maxH)
        return 0;
    if (w == W && h == H)
        return 1;

    uint16_t id = dispi_read(0);            /* index 0 = ID */
    if (id < 0xB0C0 || id > 0xB0C5)
        return 0;                            /* fara interfata dispi */

    dispi_write(4, 0);                       /* ENABLE = 0 (dezactiveaza) */
    dispi_write(1, (uint16_t)w);             /* XRES */
    dispi_write(2, (uint16_t)h);             /* YRES */
    dispi_write(3, 32);                      /* BPP */
    dispi_write(6, (uint16_t)w);             /* VIRT_WIDTH => pitch = w*4 */
    dispi_write(4, 0x41);                     /* ENABLE | LFB */
    if (dispi_read(1) != (uint16_t)w)
        return 0;                            /* n-a acceptat */

    W = w;
    H = h;
    pitch32 = w;                             /* 32bpp, pitch = latime */
    dstride = back ? W : pitch32;
    cx0 = 0; cy0 = 0; cx1 = W; cy1 = H;
    dx0 = 0; dy0 = 0; dx1 = W; dy1 = H;      /* forteaza un flush complet */
    dirty = 1;
    return 1;
}

int fb_max_width(void)  { return maxW; }
int fb_max_height(void) { return maxH; }

void fb_set_clip(int x, int y, int w, int h)
{
    cx0 = x < 0 ? 0 : x;
    cy0 = y < 0 ? 0 : y;
    cx1 = x + w > W ? W : x + w;
    cy1 = y + h > H ? H : y + h;
}

void fb_clear_clip(void)
{
    cx0 = 0;
    cy0 = 0;
    cx1 = W;
    cy1 = H;
}

int fb_active(void) { return active; }
int fb_width(void)  { return W; }
int fb_height(void) { return H; }

void fb_putpixel(int x, int y, uint32_t rgb)
{
    if (x < cx0 || y < cy0 || x >= cx1 || y >= cy1)
        return;
    draw[y * dstride + x] = rgb;
    mark(x, y, 1, 1);
}

void fb_fill(int x, int y, int w, int h, uint32_t rgb)
{
    if (x < cx0) { w -= cx0 - x; x = cx0; }
    if (y < cy0) { h -= cy0 - y; y = cy0; }
    if (x + w > cx1) w = cx1 - x;
    if (y + h > cy1) h = cy1 - y;
    if (w <= 0 || h <= 0)
        return;
    for (int j = 0; j < h; j++) {
        uint32_t *row = draw + (uint64_t)(y + j) * dstride + x;
        for (int i = 0; i < w; i++)
            row[i] = rgb;
    }
    mark(x, y, w, h);
}

/* deseneaza o imagine RGBA (0xAARRGGBB per pixel) cu alpha blending peste
 * ce e deja in back buffer. Folosit pentru iconuri anti-aliasing. */
void fb_blit_rgba(int x, int y, const uint32_t *px, int w, int h)
{
    for (int j = 0; j < h; j++) {
        int yy = y + j;
        if (yy < cy0 || yy >= cy1)
            continue;
        for (int i = 0; i < w; i++) {
            int xx = x + i;
            if (xx < cx0 || xx >= cx1)
                continue;
            uint32_t s = px[j * w + i];
            uint32_t a = s >> 24;
            if (a == 0)
                continue;
            uint32_t fg = s & 0xFFFFFF;
            if (a >= 255) {
                draw[(uint64_t)yy * dstride + xx] = fg;
            } else {
                uint32_t bg = draw[(uint64_t)yy * dstride + xx];
                draw[(uint64_t)yy * dstride + xx] = blend(fg, bg, (int)a);
            }
        }
    }
    mark(x, y, w, h);
}

void fb_copy_row(int x, int y, const uint32_t *src, int n)
{
    if (y < cy0 || y >= cy1)
        return;
    if (x < cx0) {
        src += cx0 - x;
        n -= cx0 - x;
        x = cx0;
    }
    if (x + n > cx1)
        n = cx1 - x;
    if (n <= 0)
        return;
    uint32_t *row = draw + (uint64_t)y * dstride + x;
    for (int i = 0; i < n; i++)
        row[i] = src[i];
    mark(x, y, n, 1);
}

uint32_t fb_getpixel(int x, int y)
{
    if (x < 0 || y < 0 || x >= W || y >= H)
        return 0;
    return draw[y * dstride + x];
}

static int isqrt(int v)
{
    int r = 0;
    while ((r + 1) * (r + 1) <= v)
        r++;
    return r;
}

static uint32_t blend(uint32_t fg, uint32_t bg, int a)   /* a: 0..255 */
{
    uint32_t rb = (((fg & 0xFF00FF) * (uint32_t)a +
                    (bg & 0xFF00FF) * (uint32_t)(255 - a)) >> 8) & 0xFF00FF;
    uint32_t g  = (((fg & 0x00FF00) * (uint32_t)a +
                    (bg & 0x00FF00) * (uint32_t)(255 - a)) >> 8) & 0x00FF00;
    return rb | g;
}

/* Dreptunghi cu colturi rotunde si (optional) anti-aliasing pe arce:
 * pixelii de pe marginea cercului primesc acoperire partiala, amestecati
 * fie cu o culoare solida cunoscuta (aa=1), fie cu ecranul (aa=2). */
void fb_fill_round2(int x, int y, int w, int h, int r, uint32_t rgb,
                    int aa, uint32_t under)
{
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;

    /* corpul: banda centrala + benzile de sus/jos dintre colturi */
    fb_fill(x, y + r, w, h - 2 * r, rgb);
    fb_fill(x + r, y, w - 2 * r, r, rgb);
    fb_fill(x + r, y + h - r, w - 2 * r, r, rgb);

    /* colturile, pixel cu pixel, cu acoperire dupa distanta de centru
     * (masurata in jumatati de pixel, prin centrul fiecarui pixel) */
    for (int j = 0; j < r; j++) {
        for (int i = 0; i < r; i++) {
            int a2 = 2 * (r - 1 - i) + 1;
            int b2 = 2 * (r - 1 - j) + 1;
            int d = isqrt(a2 * a2 + b2 * b2);
            int alpha = ((2 * r - d) * 255) / 2;
            if (alpha <= 0)
                continue;
            if (alpha > 255)
                alpha = 255;

            int px[4] = { x + i, x + w - 1 - i, x + i,        x + w - 1 - i };
            int py[4] = { y + j, y + j,        y + h - 1 - j, y + h - 1 - j };
            for (int k = 0; k < 4; k++) {
                if (alpha == 255 || aa == 0) {
                    if (alpha >= 128)
                        fb_putpixel(px[k], py[k], rgb);
                } else {
                    uint32_t u = (aa == 1) ? under
                                           : fb_getpixel(px[k], py[k]);
                    fb_putpixel(px[k], py[k], blend(rgb, u, alpha));
                }
            }
        }
    }
}

void fb_fill_round(int x, int y, int w, int h, int r, uint32_t rgb)
{
    fb_fill_round2(x, y, w, h, r, rgb, 0, 0);
}

void fb_char(int x, int y, char ch, uint32_t fg, uint32_t bg)
{
    /* complet in afara clipului? */
    if (x >= cx1 || y >= cy1 || x + 8 <= cx0 || y + 16 <= cy0)
        return;

    const uint8_t *g = font + (uint8_t)ch * 16;

    /* complet inauntru: calea rapida */
    if (x >= cx0 && y >= cy0 && x + 8 <= cx1 && y + 16 <= cy1) {
        for (int j = 0; j < 16; j++) {
            uint32_t *row = draw + (uint64_t)(y + j) * dstride + x;
            uint8_t bits = g[j];
            for (int i = 0; i < 8; i++)
                row[i] = (bits & (0x80 >> i)) ? fg : bg;
        }
        mark(x, y, 8, 16);
        return;
    }

    /* partial: pixel cu pixel, taiat la clip */
    for (int j = 0; j < 16; j++) {
        uint8_t bits = g[j];
        for (int i = 0; i < 8; i++)
            fb_putpixel(x + i, y + j, (bits & (0x80 >> i)) ? fg : bg);
    }
}

void fb_text(int x, int y, const char *s, uint32_t fg, uint32_t bg)
{
    while (*s) {
        fb_char(x, y, *s++, fg, bg);
        x += 8;
    }
}

void fb_text_scaled(int x, int y, const char *s, uint32_t fg, int scale)
{
    for (; *s; s++, x += 8 * scale) {
        const uint8_t *g = font + (uint8_t)*s * 16;
        for (int j = 0; j < 16; j++) {
            uint8_t bits = g[j];
            for (int i = 0; i < 8; i++)
                if (bits & (0x80 >> i))
                    fb_fill(x + i * scale, y + j * scale, scale, scale, fg);
        }
    }
}

uint32_t fb_vga_color(int idx)
{
    static const uint32_t pal[16] = {
        0x000000, 0x0000AA, 0x00AA00, 0x00AAAA,
        0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
        0x555555, 0x5555FF, 0x55FF55, 0x55FFFF,
        0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF,
    };
    return pal[idx & 0xF];
}
