#include <stdbool.h>
#include "keyboard.h"
#include "io.h"
#include "interrupts.h"
#include "vga.h"
#include "gui.h"

#define KBD_DATA 0x60

/* Scancode set 1, layout US. 0 = tasta nu produce caracter (Ctrl, F1...).
 * Caps Lock nu e implementat inca. */
static const char keymap[64] = {
    0,  27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0,  'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0, '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0,
    '*', 0, ' ',
};

static const char keymap_shift[64] = {
    0,  27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
    '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    0,  'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
    0,  '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0,
    '*', 0, ' ',
};

static bool shift;
static bool alt;
static bool ctrl;
static bool caps;   /* Caps Lock activ */
static bool e0;   /* am primit prefixul 0xE0 (taste extinse) */

static void kb_irq(struct int_frame *f)
{
    (void)f;
    uint8_t sc = inb(KBD_DATA);

    if (sc == 0xE0) {
        e0 = true;
        return;
    }
    if (e0) {
        e0 = false;
        if (sc & 0x80)
            return;                     /* eliberare de tasta extinsa */
        if (sc == 0x5B || sc == 0x5C) { /* tasta Windows/Super (stanga/dreapta) */
            gui_menu_toggle();
            return;
        }
        /* sagetile si Delete/Home/End devin coduri speciale >= 0x80;
         * cu Shift apasat, variantele de selectie 0x90..0x95 */
        char spec = 0;
        switch (sc) {
        case 0x48: spec = shift ? (char)0x90 : (char)0x80; break;   /* sus */
        case 0x50: spec = shift ? (char)0x91 : (char)0x81; break;   /* jos */
        case 0x4B: spec = shift ? (char)0x92 : (char)0x82; break;   /* stanga */
        case 0x4D: spec = shift ? (char)0x93 : (char)0x83; break;   /* dreapta */
        case 0x53: spec = (char)0x84; break;                        /* Delete */
        case 0x47: spec = shift ? (char)0x94 : (char)0x85; break;   /* Home */
        case 0x4F: spec = shift ? (char)0x95 : (char)0x86; break;   /* End */
        }
        if (spec && !gui_key_intercept(spec))
            console_push_key(spec);
        return;
    }

    bool release = sc & 0x80;
    uint8_t code = sc & 0x7F;

    if (code == 0x38) {                   /* Alt stanga */
        alt = !release;
        return;
    }
    if (code == 0x1D) {                   /* Ctrl stanga */
        ctrl = !release;
        return;
    }
    if (code == 0x2A || code == 0x36) {   /* Shift stanga / dreapta */
        shift = !release;
        return;
    }
    if (code == 0x3A) {                    /* Caps Lock: comuta la apasare */
        if (!release)
            caps = !caps;
        return;
    }

    /* Alt+F1..F3: comuta la terminal daca e deschis, altfel il deschide */
    if (alt && !release && code >= 0x3B && code < 0x3B + CON_COUNT) {
        int n = code - 0x3B;
        if (gui_terminal_is_open(n))
            console_switch(n);
        else
            gui_request_terminal(n);
        return;
    }
    if (alt && !release && code == 0x3B + CON_COUNT) {
        gui_fm_toggle();
        return;
    }
    if (alt && !release && code == 0x3C + CON_COUNT) {   /* Alt+F5 */
        gui_np_toggle();
        return;
    }
    if (alt && !release && code == 0x3D + CON_COUNT) {   /* Alt+F6 */
        gui_tm_toggle();
        return;
    }
    if (alt && !release && code == 0x3E + CON_COUNT) {   /* Alt+F7 */
        gui_br_toggle();
        return;
    }
    if (alt && !release && code == 0x3F + CON_COUNT) {   /* Alt+F8 */
        gui_set_toggle();
        return;
    }
    if (alt && !release && code == 0x40 + CON_COUNT) {   /* Alt+F9 */
        gui_calc_toggle();
        return;
    }
    if (!release && code == 0x57) {      /* F11 = maximizeaza/restaureaza fereastra */
        gui_maximize_focused();
        return;
    }

    /* F1..F10 simple (fara Alt) -> coduri abstracte 0xB0..0xB9 pentru terminal
     * (ssh le traduce in secvente ANSI pt. mc/nano/etc.) */
    if (!release && !alt && code >= 0x3B && code <= 0x44) {
        char fk = (char)(0xB0 + (code - 0x3B));
        if (!gui_key_intercept(fk))
            console_push_key(fk);
        return;
    }

    if (release || code >= sizeof(keymap))
        return;

    char c = shift ? keymap_shift[code] : keymap[code];
    /* Caps Lock afecteaza doar literele (XOR cu Shift) */
    if (caps) {
        if (c >= 'a' && c <= 'z')      c = (char)(c - 'a' + 'A');
        else if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    }
    /* Ctrl+litera -> cod de control (1..26), ex. Ctrl+S=0x13 */
    if (ctrl && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')))
        c = (char)((c | 0x20) - 'a' + 1);
    if (c && !gui_key_intercept(c))
        console_push_key(c);   /* la terminalul activ */
}

void keyboard_init(void)
{
    irq_install(1, kb_irq);
    (void)inb(KBD_DATA);   /* golim un eventual scancode ramas in buffer */
}
