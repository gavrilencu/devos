#include "vga.h"
#include "io.h"
#include "task.h"
#include "fb.h"
#include "gui.h"
#include "string.h"

#define VGA_MEM   ((volatile uint16_t *)0xB8000)
#define VGA_W     80
#define VGA_H     25
#define CRTC_IDX  0x3D4
#define CRTC_DATA 0x3D5

/* Serializare cu IRQ-urile: desenarea consolei (context de task) nu trebuie
 * intrerupta de IRQ-ul de mouse (care deseneaza cursorul) sau de timer, ca
 * sa nu se corupa framebuffer-ul. */
static inline uint64_t fb_lock(void)
{
    uint64_t fl;
    __asm__ volatile("pushfq; cli; pop %0" : "=r"(fl));
    return fl;
}
static inline void fb_unlock(uint64_t fl)
{
    __asm__ volatile("push %0; popfq" : : "r"(fl) : "cc");
}

#define KEY_BUF 256

struct console {
    uint16_t buf[VGA_W * VGA_H];   /* copia proprie a ecranului */
    int row, col;
    uint8_t color;
    /* coada de tastatura: IRQ-ul scrie head, consumatorul scrie tail */
    char keys[KEY_BUF];
    volatile unsigned head, tail;
    /* interpretor de secvente ANSI/VT100 (pentru ssh: mc, nano, ls --color) */
    int esc;                       /* 0 normal,1 dupa ESC,2 CSI,3 OSC,4 G0,5 ST,6 G1 */
    int params[8], nparam, priv, pany;
    uint8_t bright;                /* atribut bold curent pt. SGR */
    int s_top, s_bot;              /* regiune de scroll (0-based) */
    int saved_row, saved_col;
    int g0_gfx;                    /* G0 = charset de linii DEC (ESC ( 0) */
    int utf_need;                  /* octeti UTF-8 ramasi de citit */
    unsigned utf_cp;               /* codepoint UTF-8 in curs de asamblare */
    int app_cursor;                /* DECCKM: sagetile trimit \eO.. (nu \e[..) */
};

static struct console cons[CON_COUNT];
static int active;

/* Mod grafic (setat de stage2): nu mai atingem memoria text 0xB8000 —
 * in modul VESA regiunea legacy poate cadea peste framebuffer. `render`
 * porneste abia dupa ce desktopul e desenat. */
static int gfx;
static int render;
static int cur_r = -1, cur_c = -1;   /* pozitia cursorului "soft" desenat */

static uint16_t cell(struct console *c, char ch)
{
    return (uint16_t)((uint16_t)c->color << 8 | (uint8_t)ch);
}

/* deseneaza o celula a unui terminal in fereastra lui, daca nu e
 * acoperita de o fereastra aflata deasupra */
static void fb_cell(int term, int r, int cidx)
{
    int x = gui_win_x(term) + cidx * 8;
    int y = gui_win_y(term) + r * 16;
    if (!gui_cell_visible(term, x, y))
        return;
    uint16_t v = cons[term].buf[r * VGA_W + cidx];
    fb_char(x, y, (char)(v & 0xFF),
            fb_vga_color((v >> 8) & 0xF), fb_vga_color((v >> 12) & 0xF));
}

static void sync_cursor(void)
{
    struct console *c = &cons[active];

    if (gfx) {
        if (!render)
            return;
        if (cur_r >= 0)
            fb_cell(active, cur_r, cur_c);   /* stergem cursorul vechi */
        cur_r = c->row < VGA_H ? c->row : VGA_H - 1;
        cur_c = c->col < VGA_W ? c->col : VGA_W - 1;
        int x = gui_win_x(active) + cur_c * 8;
        int y = gui_win_y(active) + cur_r * 16;
        if (gui_cell_visible(active, x, y))
            fb_fill(x, y + 14, 8, 2, 0xB8C4D8);
        return;
    }

    uint16_t pos = (uint16_t)(c->row * VGA_W + c->col);
    outb(CRTC_IDX, 0x0F);
    outb(CRTC_DATA, (uint8_t)(pos & 0xFF));
    outb(CRTC_IDX, 0x0E);
    outb(CRTC_DATA, (uint8_t)(pos >> 8));
}

void console_repaint_term(int term)
{
    if (!gfx || !render)
        return;
    uint64_t fl = fb_lock();
    for (int r = 0; r < VGA_H; r++)
        for (int i = 0; i < VGA_W; i++)
            fb_cell(term, r, i);
    if (term == active) {
        cur_r = -1;
        sync_cursor();
    }
    fb_unlock(fl);
}

/* redeseneaza tot ecranul terminalului (in grafic: fereastra lui) */
static void blit(struct console *c)
{
    if (gfx) {
        console_repaint_term((int)(c - cons));
        return;
    }
    if (c != &cons[active])
        return;
    memcpy((void *)VGA_MEM, c->buf, sizeof(c->buf));
}

void console_clear(int t)
{
    struct console *c = &cons[t];
    c->row = 0;
    c->col = 0;
    c->esc = 0;
    c->nparam = 0;
    c->pany = 0;
    c->priv = 0;
    c->bright = 0;
    c->s_top = 0;
    c->s_bot = VGA_H - 1;
    c->saved_row = 0;
    c->saved_col = 0;
    c->g0_gfx = 0;
    c->utf_need = 0;
    c->utf_cp = 0;
    c->app_cursor = 0;
    for (int i = 0; i < VGA_W * VGA_H; i++)
        c->buf[i] = cell(c, ' ');
    blit(c);
    if (c == &cons[active])
        sync_cursor();
}

void console_set_color(int t, enum vga_color fg, enum vga_color bg)
{
    cons[t].color = (uint8_t)(bg << 4 | fg);
}

/* deseneaza o celula (r,cidx) din bufferul terminalului t, daca draw */
static void putcell(int t, int r, int cidx, int draw)
{
    if (!draw)
        return;
    if (gfx)
        fb_cell(t, r, cidx);
    else if (t == active)
        VGA_MEM[r * VGA_W + cidx] = cons[t].buf[r * VGA_W + cidx];
}

static void redraw_rows(int t, int top, int bot, int draw)
{
    if (!draw)
        return;
    for (int r = top; r <= bot; r++)
        for (int x = 0; x < VGA_W; x++)
            putcell(t, r, x, 1);
}

/* scroll in sus in interiorul regiunii [s_top, s_bot] (continut urca) */
static void scroll_up_region(struct console *c, int t, int draw)
{
    int top = c->s_top, bot = c->s_bot;
    memmove(c->buf + top * VGA_W, c->buf + (top + 1) * VGA_W,
            (bot - top) * VGA_W * sizeof(uint16_t));
    for (int x = 0; x < VGA_W; x++)
        c->buf[bot * VGA_W + x] = cell(c, ' ');
    redraw_rows(t, top, bot, draw);
}

/* scroll in jos (reverse index): continut coboara, rand gol sus */
static void scroll_down_region(struct console *c, int t, int draw)
{
    int top = c->s_top, bot = c->s_bot;
    for (int r = bot; r > top; r--)
        memmove(c->buf + r * VGA_W, c->buf + (r - 1) * VGA_W,
                VGA_W * sizeof(uint16_t));
    for (int x = 0; x < VGA_W; x++)
        c->buf[top * VGA_W + x] = cell(c, ' ');
    redraw_rows(t, top, bot, draw);
}

static void erase_cells(struct console *c, int t, int from, int to, int draw)
{
    if (from < 0) from = 0;
    if (to > VGA_W * VGA_H) to = VGA_W * VGA_H;
    for (int i = from; i < to; i++)
        c->buf[i] = cell(c, ' ');
    if (draw)
        for (int i = from; i < to; i++)
            putcell(t, i / VGA_W, i % VGA_W, 1);
}

/* insereaza n randuri goale la randul curent (in regiune) */
static void insert_lines(struct console *c, int t, int n, int draw)
{
    int top = c->row, bot = c->s_bot;
    if (top < c->s_top) top = c->s_top;
    if (top > bot) return;
    if (n > bot - top + 1) n = bot - top + 1;
    for (int r = bot; r >= top + n; r--)
        memmove(c->buf + r * VGA_W, c->buf + (r - n) * VGA_W, VGA_W * sizeof(uint16_t));
    for (int r = top; r < top + n; r++)
        for (int x = 0; x < VGA_W; x++)
            c->buf[r * VGA_W + x] = cell(c, ' ');
    redraw_rows(t, top, bot, draw);
}

static void delete_lines(struct console *c, int t, int n, int draw)
{
    int top = c->row, bot = c->s_bot;
    if (top < c->s_top) top = c->s_top;
    if (top > bot) return;
    if (n > bot - top + 1) n = bot - top + 1;
    for (int r = top; r <= bot - n; r++)
        memmove(c->buf + r * VGA_W, c->buf + (r + n) * VGA_W, VGA_W * sizeof(uint16_t));
    for (int r = bot - n + 1; r <= bot; r++)
        for (int x = 0; x < VGA_W; x++)
            c->buf[r * VGA_W + x] = cell(c, ' ');
    redraw_rows(t, top, bot, draw);
}

static void delete_chars(struct console *c, int t, int n, int draw)
{
    int r = c->row, s = c->col;
    if (n > VGA_W - s) n = VGA_W - s;
    for (int x = s; x < VGA_W - n; x++)
        c->buf[r * VGA_W + x] = c->buf[r * VGA_W + x + n];
    for (int x = VGA_W - n; x < VGA_W; x++)
        c->buf[r * VGA_W + x] = cell(c, ' ');
    if (draw)
        for (int x = s; x < VGA_W; x++) putcell(t, r, x, 1);
}

static void insert_chars(struct console *c, int t, int n, int draw)
{
    int r = c->row, s = c->col;
    if (n > VGA_W - s) n = VGA_W - s;
    for (int x = VGA_W - 1; x >= s + n; x--)
        c->buf[r * VGA_W + x] = c->buf[r * VGA_W + x - n];
    for (int x = s; x < s + n; x++)
        c->buf[r * VGA_W + x] = cell(c, ' ');
    if (draw)
        for (int x = s; x < VGA_W; x++) putcell(t, r, x, 1);
}

/* SGR: culori/atribute ANSI -> atribut VGA (bg<<4|fg) */
static const uint8_t ansi2vga[8] = { 0, 4, 2, 6, 1, 5, 3, 7 };
static void apply_sgr(struct console *c, int nused)
{
    int fg = c->color & 0x0F, bg = (c->color >> 4) & 0x0F;
    if (nused == 0) { c->params[0] = 0; nused = 1; }
    for (int i = 0; i < nused; i++) {
        int p = c->params[i];
        if (p == 0)               { fg = VGA_LIGHT_GREY; bg = VGA_BLACK; c->bright = 0; }
        else if (p == 1)          { c->bright = 8; fg |= 8; }
        else if (p == 22)         { c->bright = 0; fg &= 7; }
        else if (p == 7)          { int tmp = fg; fg = bg; bg = tmp; }
        else if (p >= 30 && p <= 37) fg = ansi2vga[p - 30] | c->bright;
        else if (p == 38)         { if (i+1<nused && c->params[i+1]==5) i+=2; else if (i+1<nused && c->params[i+1]==2) i+=4; fg = VGA_LIGHT_GREY; }
        else if (p == 39)         fg = VGA_LIGHT_GREY;
        else if (p >= 40 && p <= 47) bg = ansi2vga[p - 40];
        else if (p == 48)         { if (i+1<nused && c->params[i+1]==5) i+=2; else if (i+1<nused && c->params[i+1]==2) i+=4; bg = VGA_BLACK; }
        else if (p == 49)         bg = VGA_BLACK;
        else if (p >= 90 && p <= 97)   fg = ansi2vga[p - 90] | 8;
        else if (p >= 100 && p <= 107) bg = ansi2vga[p - 100];
    }
    c->color = (uint8_t)((bg << 4) | fg);
}

/* trateaza un octet final CSI (ESC [ ... litera) */
static void csi_dispatch(struct console *c, int t, char fin, int draw)
{
    int nused = c->pany ? c->nparam + 1 : 0;
    int *p = c->params;
    int n = (nused >= 1 && p[0] > 0) ? p[0] : 1;
    switch (fin) {
    case 'H': case 'f': {
        int r = (nused >= 1 && p[0] > 0) ? p[0] - 1 : 0;
        int col = (nused >= 2 && p[1] > 0) ? p[1] - 1 : 0;
        if (r < 0) r = 0; if (r >= VGA_H) r = VGA_H - 1;
        if (col < 0) col = 0; if (col >= VGA_W) col = VGA_W - 1;
        c->row = r; c->col = col; break;
    }
    case 'A': c->row -= n; if (c->row < 0) c->row = 0; break;
    case 'B': c->row += n; if (c->row >= VGA_H) c->row = VGA_H - 1; break;
    case 'C': c->col += n; if (c->col >= VGA_W) c->col = VGA_W - 1; break;
    case 'D': c->col -= n; if (c->col < 0) c->col = 0; break;
    case 'E': c->col = 0; c->row += n; if (c->row >= VGA_H) c->row = VGA_H - 1; break;
    case 'F': c->col = 0; c->row -= n; if (c->row < 0) c->row = 0; break;
    case 'G': case '`': c->col = (n < 1 ? 0 : n - 1); if (c->col >= VGA_W) c->col = VGA_W - 1; break;
    case 'd': c->row = (n < 1 ? 0 : n - 1); if (c->row >= VGA_H) c->row = VGA_H - 1; break;
    case 'J': {
        int m = (nused >= 1) ? p[0] : 0;
        int cur = c->row * VGA_W + c->col;
        if (m == 0)      erase_cells(c, t, cur, VGA_W * VGA_H, draw);
        else if (m == 1) erase_cells(c, t, 0, cur + 1, draw);
        else             erase_cells(c, t, 0, VGA_W * VGA_H, draw);
        break;
    }
    case 'K': {
        int m = (nused >= 1) ? p[0] : 0;
        int rs = c->row * VGA_W;
        if (m == 0)      erase_cells(c, t, rs + c->col, rs + VGA_W, draw);
        else if (m == 1) erase_cells(c, t, rs, rs + c->col + 1, draw);
        else             erase_cells(c, t, rs, rs + VGA_W, draw);
        break;
    }
    case 'X': { int cur = c->row * VGA_W + c->col; erase_cells(c, t, cur, cur + n, draw); break; }
    case 'm': apply_sgr(c, nused); break;
    case 'r': {
        int top = (nused >= 1 && p[0] > 0) ? p[0] - 1 : 0;
        int bot = (nused >= 2 && p[1] > 0) ? p[1] - 1 : VGA_H - 1;
        if (top < 0) top = 0; if (bot >= VGA_H) bot = VGA_H - 1;
        if (top >= bot) { top = 0; bot = VGA_H - 1; }
        c->s_top = top; c->s_bot = bot; c->row = top; c->col = 0; break;
    }
    case 'L': insert_lines(c, t, n, draw); break;
    case 'M': delete_lines(c, t, n, draw); break;
    case 'P': delete_chars(c, t, n, draw); break;
    case '@': insert_chars(c, t, n, draw); break;
    case 's': c->saved_row = c->row; c->saved_col = c->col; break;
    case 'u': c->row = c->saved_row; c->col = c->saved_col; break;
    case 'h': case 'l': {
        int set = (fin == 'h');
        if (c->priv)
            for (int i = 0; i < nused; i++) {
                if (p[i] == 1) c->app_cursor = set;        /* DECCKM */
                else if (p[i] == 1049 || p[i] == 47 || p[i] == 1047) {
                    erase_cells(c, t, 0, VGA_W * VGA_H, draw); c->row = 0; c->col = 0;
                }
            }
        break;
    }
    default: break;
    }
}

/* dupa ESC (esc==1): octet intermediar / secventa scurta */
static void esc_after(struct console *c, int t, char ch, int draw)
{
    switch (ch) {
    case '[':
        c->esc = 2; c->nparam = 0; c->priv = 0; c->pany = 0;
        for (int i = 0; i < 8; i++) c->params[i] = 0;
        return;
    case ']': c->esc = 3; return;                 /* OSC (titlu) */
    case '(': c->esc = 4; return;                 /* selectie G0 */
    case ')': case '*': case '+': c->esc = 6; return;  /* G1/G2/G3 (ignorat) */
    case '=': case '>': c->esc = 0; return;       /* mod keypad */
    case 'M': if (c->row <= c->s_top) scroll_down_region(c, t, draw); else c->row--; c->esc = 0; return;
    case 'D': if (c->row >= c->s_bot) scroll_up_region(c, t, draw); else c->row++; c->esc = 0; return;
    case 'E': c->col = 0; if (c->row >= c->s_bot) scroll_up_region(c, t, draw); else c->row++; c->esc = 0; return;
    case '7': c->saved_row = c->row; c->saved_col = c->col; c->esc = 0; return;
    case '8': c->row = c->saved_row; c->col = c->saved_col; c->esc = 0; return;
    case 'c': c->color = (uint8_t)(VGA_BLACK << 4 | VGA_LIGHT_GREY);
              c->s_top = 0; c->s_bot = VGA_H - 1;
              erase_cells(c, t, 0, VGA_W * VGA_H, draw); c->row = 0; c->col = 0; c->esc = 0; return;
    default: c->esc = 0; return;
    }
}

/* mapeaza un codepoint Unicode (box-drawing, blocuri, simboluri) la un
 * octet din fontul CP437 al BIOS-ului (de la 0x6000). */
static unsigned char map_unicode(unsigned cp)
{
    switch (cp) {
    case 0x2500: case 0x2501: return 0xC4;  /* ─ */
    case 0x2502: case 0x2503: return 0xB3;  /* │ */
    case 0x250C: case 0x250F: return 0xDA;  /* ┌ */
    case 0x2510: case 0x2513: return 0xBF;  /* ┐ */
    case 0x2514: case 0x2517: return 0xC0;  /* └ */
    case 0x2518: case 0x251B: return 0xD9;  /* ┘ */
    case 0x251C: case 0x2523: return 0xC3;  /* ├ */
    case 0x2524: case 0x252B: return 0xB4;  /* ┤ */
    case 0x252C: case 0x2533: return 0xC2;  /* ┬ */
    case 0x2534: case 0x253B: return 0xC1;  /* ┴ */
    case 0x253C: case 0x254B: return 0xC5;  /* ┼ */
    case 0x2550: return 0xCD;               /* ═ */
    case 0x2551: return 0xBA;               /* ║ */
    case 0x2552: case 0x2553: case 0x2554: return 0xC9;  /* ╔ */
    case 0x2555: case 0x2556: case 0x2557: return 0xBB;  /* ╗ */
    case 0x2558: case 0x2559: case 0x255A: return 0xC8;  /* ╚ */
    case 0x255B: case 0x255C: case 0x255D: return 0xBC;  /* ╝ */
    case 0x2560: return 0xCC;  case 0x2563: return 0xB9;
    case 0x2566: return 0xCB;  case 0x2569: return 0xCA;
    case 0x256C: return 0xCE;
    case 0x2580: return 0xDF;  /* ▀ */
    case 0x2584: return 0xDC;  /* ▄ */
    case 0x2588: return 0xDB;  /* █ */
    case 0x258C: return 0xDD;  /* ▌ */
    case 0x2590: return 0xDE;  /* ▐ */
    case 0x2591: return 0xB0;  /* ░ */
    case 0x2592: return 0xB1;  /* ▒ */
    case 0x2593: return 0xB2;  /* ▓ */
    case 0x25A0: return 0xFE;  case 0x25AC: return 0xDB;
    case 0x2022: return 0x07;  /* • */
    case 0x00B7: return 0xFA;  /* · */
    case 0x2026: return '.';   /* … */
    case 0x2190: return 0x1B;  case 0x2191: return 0x18;
    case 0x2192: return 0x1A;  case 0x2193: return 0x19;
    case 0x00B0: return 0xF8;  case 0x00B1: return 0xF1;
    default:
        if (cp < 0x80) return (unsigned char)cp;
        if (cp >= 0x2500 && cp <= 0x257F) return 0xC5;  /* alte linii -> ┼ */
        if (cp >= 0x2580 && cp <= 0x259F) return 0xDB;  /* blocuri -> █ */
        return '?';
    }
}

/* charset DEC de linii (ESC ( 0): ASCII 0x60..0x7E -> glife CP437 */
static const unsigned char vt100_gfx[31] = {
    0x04, 0xB1, 0x09, 0x0C, 0x0D, 0x0A, 0xF8, 0xF1, 0x23, 0x23, 0xD9, 0xBF,
    0xDA, 0xC0, 0xC5, 0x7E, 0xC4, 0xC4, 0xC4, 0x5F, 0xC3, 0xB4, 0xC1, 0xC2,
    0xB3, 0xF3, 0xF2, 0xE3, 0xD8, 0x9C, 0xFA
};

/* scrie un octet la cursor, avanseaza, cu wrap si scroll de regiune */
static void emit_byte(struct console *c, int t, unsigned char b, int draw)
{
    c->buf[c->row * VGA_W + c->col] = cell(c, (char)b);
    putcell(t, c->row, c->col, draw);
    if (++c->col >= VGA_W) {
        c->col = 0;
        if (c->row >= c->s_bot) scroll_up_region(c, t, draw);
        else c->row++;
    }
}

static void normal_char(struct console *c, int t, char ch, int draw)
{
    unsigned char b = (unsigned char)ch;

    /* decodare UTF-8 (mc/ncurses trimit box-drawing in UTF-8) */
    if (c->utf_need > 0) {
        if ((b & 0xC0) == 0x80) {
            c->utf_cp = (c->utf_cp << 6) | (b & 0x3F);
            if (--c->utf_need == 0)
                emit_byte(c, t, map_unicode(c->utf_cp), draw);
            return;
        }
        c->utf_need = 0;                 /* secventa invalida: reia normal */
    }
    if (b >= 0xC0) {                      /* inceput de secventa UTF-8 */
        if (b >= 0xF0)      { c->utf_need = 3; c->utf_cp = b & 0x07; }
        else if (b >= 0xE0) { c->utf_need = 2; c->utf_cp = b & 0x0F; }
        else                { c->utf_need = 1; c->utf_cp = b & 0x1F; }
        return;
    }

    if (ch == '\n') {
        /* DevOS trateaza '\n' ca CR+LF (programele locale trimit doar '\n';
         * ssh/pty trimit "\r\n", deci CR-ul e redundant, nu strica) */
        c->col = 0;
        if (c->row >= c->s_bot) scroll_up_region(c, t, draw);
        else c->row++;
    } else if (ch == '\r') {
        c->col = 0;
    } else if (ch == '\b') {
        if (c->col > 0) {
            c->col--;
            c->buf[c->row * VGA_W + c->col] = cell(c, ' ');
            putcell(t, c->row, c->col, draw);
        }
    } else if (ch == '\t') {
        int nc = (c->col + 8) & ~7;
        if (nc >= VGA_W) nc = VGA_W - 1;
        c->col = nc;
    } else if (b >= 32 && b < 127) {
        unsigned char out = b;
        if (c->g0_gfx && b >= 0x60 && b <= 0x7E)
            out = vt100_gfx[b - 0x60];   /* charset de linii DEC */
        emit_byte(c, t, out, draw);
    } else if (b >= 0x80) {
        emit_byte(c, t, b, draw);        /* octet CP437 brut (fallback) */
    }
    /* alte coduri de control (BEL etc.) sunt ignorate */
}

void console_putc(int t, char ch)
{
    struct console *c = &cons[t];
    int is_act = (c == &cons[active]);
    /* in grafic, fiecare terminal isi deseneaza LIVE celulele in
     * fereastra lui (chiar daca nu e focusat) */
    int draw = gfx ? render : is_act;

    uint64_t fl = fb_lock();       /* atomic fata de IRQ-ul de mouse/timer */

    if (c->esc == 1) {
        esc_after(c, t, ch, draw);
    } else if (c->esc == 2) {                     /* CSI: aduna parametri */
        if (ch == '?') {
            c->priv = 1;
        } else if (ch >= '0' && ch <= '9') {
            if (c->nparam < 8) c->params[c->nparam] = c->params[c->nparam] * 10 + (ch - '0');
            c->pany = 1;
        } else if (ch == ';') {
            if (c->nparam < 7) c->nparam++;
            c->pany = 1;
        } else if (ch >= 0x40 && ch <= 0x7E) {    /* octet final */
            csi_dispatch(c, t, ch, draw);
            c->esc = 0;
        }
        /* octetii intermediari (0x20-0x2F) sunt ignorati, ramanem in CSI */
    } else if (c->esc == 3) {                     /* OSC: pana la BEL sau ESC */
        if (ch == 0x07) c->esc = 0;
        else if (ch == 0x1B) c->esc = 5;
    } else if (c->esc == 4) {                     /* selectie G0: ESC ( <F> */
        c->g0_gfx = (ch == '0');                  /* '0'=linii DEC, 'B'=ASCII */
        c->esc = 0;
    } else if (c->esc == 5) {                     /* ST dupa OSC (ESC \) */
        c->esc = 0;
    } else if (c->esc == 6) {                     /* selectie G1/G2/G3: ignora */
        c->esc = 0;
    } else if (ch == 0x1B) {
        c->esc = 1;
    } else {
        normal_char(c, t, ch, draw);
    }

    if (is_act)
        sync_cursor();

    fb_unlock(fl);
}

void console_set_active(int t)
{
    if (t < 0 || t >= CON_COUNT || t == active)
        return;
    if (gfx && render && cur_r >= 0)
        fb_cell(active, cur_r, cur_c);   /* stergem cursorul vechi */
    active = t;
    cur_r = -1;
    sync_cursor();
}

void console_switch(int t)
{
    if (t < 0 || t >= CON_COUNT)
        return;

    if (gfx) {
        if (render)
            gui_focus_notify(t);   /* WM seteaza activul si redeseneaza */
        else
            console_set_active(t);
        return;
    }

    if (t == active)
        return;
    active = t;
    blit(&cons[t]);
    sync_cursor();
}

int console_active(void)
{
    return active;
}

int console_app_cursor(int t)
{
    if (t < 0 || t >= CON_COUNT)
        return 0;
    return cons[t].app_cursor;
}

void console_enable_render(void)
{
    render = 1;   /* celulele le deseneaza window manager-ul (win_draw) */
}

void console_init(void)
{
    /* stage2 lasa flagul de mod grafic la 0x5F07 (vezi boot/stage2.asm);
     * il citim de aici ca sa nu atingem 0xB8000 nici macar la primul
     * kprintf — suntem DEJA in mod grafic cand porneste kernelul. */
    gfx = (*(volatile uint8_t *)0x5F07 == 1);

    for (int t = CON_COUNT - 1; t >= 0; t--) {
        cons[t].color = (uint8_t)(VGA_BLACK << 4 | VGA_LIGHT_GREY);
        console_clear(t);
    }
    active = 0;
    blit(&cons[0]);
    sync_cursor();
}

void console_push_key_to(int term, char ch)
{
    struct console *c = &cons[term];
    unsigned next = (c->head + 1) % KEY_BUF;
    if (next != c->tail) {           /* coada plina: pierdem tasta */
        c->keys[c->head] = ch;
        c->head = next;
    }
}

void console_push_key(char ch)
{
    console_push_key_to(active, ch);
}

int console_getchar(int t)
{
    struct console *c = &cons[t];
    if (c->head == c->tail)
        return -1;
    char ch = c->keys[c->tail];
    c->tail = (c->tail + 1) % KEY_BUF;
    return (unsigned char)ch;
}

void con_putc(char ch)
{
    console_putc(task_current_term(), ch);
}

void con_color(enum vga_color fg, enum vga_color bg)
{
    console_set_color(task_current_term(), fg, bg);
}

void con_clear(void)
{
    console_clear(task_current_term());
}

void vga_write_at(int r, int c, const char *s,
                  enum vga_color fg, enum vga_color bg)
{
    if (gfx) {
        if (render)
            fb_text(gui_win_x(active) + c * 8, gui_win_y(active) + r * 16,
                    s, fb_vga_color(fg), fb_vga_color(bg));
        return;
    }
    uint8_t attr = (uint8_t)(bg << 4 | fg);
    while (*s && c < VGA_W)
        VGA_MEM[r * VGA_W + c++] = (uint16_t)attr << 8 | (uint8_t)*s++;
}
