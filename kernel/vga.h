#pragma once
#include <stdint.h>

/* Terminale virtuale peste modul text VGA (80x25, buffer la 0xB8000).
 * Fiecare terminal are ecran, cursor, culoare si coada de tastatura
 * proprii; cel activ e oglindit in memoria VGA. Comutare: Alt+F1..F3. */

enum vga_color {
    VGA_BLACK = 0,
    VGA_BLUE,
    VGA_GREEN,
    VGA_CYAN,
    VGA_RED,
    VGA_MAGENTA,
    VGA_BROWN,
    VGA_LIGHT_GREY,
    VGA_DARK_GREY,
    VGA_LIGHT_BLUE,
    VGA_LIGHT_GREEN,
    VGA_LIGHT_CYAN,
    VGA_LIGHT_RED,
    VGA_LIGHT_MAGENTA,
    VGA_YELLOW,
    VGA_WHITE,
};

#define CON_COUNT 3

void console_init(void);
void console_switch(int term);
int console_active(void);

/* Mod grafic: enter_gfx (chemat automat la init daca stage2 a setat VBE)
 * opreste scrierile in memoria text 0xB8000; enable_render porneste
 * desenarea celulelor pe framebuffer (dupa ce desktopul e desenat). */
void console_enable_render(void);

/* redeseneaza toate celulele unui terminal in fereastra lui (window
 * manager-ul o cheama cand fereastra e mutata/expusa) */
void console_repaint_term(int term);

/* operatii pe un terminal anume */
void console_putc(int term, char c);
void console_clear(int term);
void console_set_color(int term, enum vga_color fg, enum vga_color bg);

/* wrappere pe terminalul task-ului curent */
void con_putc(char c);
void con_color(enum vga_color fg, enum vga_color bg);
void con_clear(void);

/* tastatura: driverul impinge in terminalul ACTIV; task-urile citesc
 * fiecare din terminalul LUI */
void console_push_key(char c);
void console_push_key_to(int term, char c);   /* injectie directa (ex. FM) */
int console_getchar(int term);   /* -1 daca nu e nimic */

/* schimba terminalul care primeste tastatura + cursorul, FARA sa anunte
 * window manager-ul (folosit chiar de WM la schimbarea focusului) */
void console_set_active(int term);

/* scrie direct in memoria VGA, peste orice terminal e afisat —
 * pentru elementele globale de UI (uptime, spinner) */
void vga_write_at(int row, int col, const char *s,
                  enum vga_color fg, enum vga_color bg);
