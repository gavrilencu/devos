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

static void scroll(struct console *c)
{
    memmove(c->buf, c->buf + VGA_W, (VGA_H - 1) * VGA_W * sizeof(uint16_t));
    for (int x = 0; x < VGA_W; x++)
        c->buf[(VGA_H - 1) * VGA_W + x] = cell(c, ' ');
    c->row = VGA_H - 1;
    blit(c);
}

void console_putc(int t, char ch)
{
    struct console *c = &cons[t];
    int is_act = (c == &cons[active]);
    /* in grafic, fiecare terminal isi deseneaza LIVE celulele in
     * fereastra lui (chiar daca nu e focusat) */
    int draw = gfx ? render : is_act;

    uint64_t fl = fb_lock();       /* atomic fata de IRQ-ul de mouse/timer */

    if (ch == '\n') {
        c->col = 0;
        c->row++;
    } else if (ch == '\r') {
        c->col = 0;
    } else if (ch == '\b') {
        if (c->col > 0) {
            c->col--;
            c->buf[c->row * VGA_W + c->col] = cell(c, ' ');
            if (draw) {
                if (gfx)
                    fb_cell(t, c->row, c->col);
                else
                    VGA_MEM[c->row * VGA_W + c->col] = cell(c, ' ');
            }
        }
    } else {
        c->buf[c->row * VGA_W + c->col] = cell(c, ch);
        if (draw) {
            if (gfx)
                fb_cell(t, c->row, c->col);
            else
                VGA_MEM[c->row * VGA_W + c->col] = cell(c, ch);
        }
        if (++c->col == VGA_W) {
            c->col = 0;
            c->row++;
        }
    }

    if (c->row == VGA_H)
        scroll(c);
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
