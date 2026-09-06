#pragma once
#include <stdint.h>

/* Interfata grafica: splash, desktop (taskbar cu tab-uri + ceas RTC,
 * meniu Start), cursor de mouse si un window manager: fiecare terminal
 * are propria fereastra, mutabila cu mouse-ul, cu z-order si focus.
 * Consola (80x25 celule de 8x16 px) e desenata de vga.c in fereastra
 * terminalului respectiv. */

void splash_show(void);
void splash_progress(int pct);          /* 0..100 */
void splash_animate(void);              /* bara + spinner rotitor, ~2s */

void desktop_draw(int active_term);     /* wallpaper + taskbar */
void gui_windows_open(void);            /* deseneaza ferestrele (cu animatie) */
void gui_desktop_ready(void);           /* activeaza mouse-ul/hoverul */

/* chemat de vga.c cand se schimba terminalul focusat: ridica fereastra
 * in fata si redeseneaza chrome-ul + tab-urile */
void gui_focus_notify(int term);

/* pozitia continutului ferestrei unui terminal si vizibilitatea unei
 * celule de text (ocluzie de ferestrele de deasupra / meniul Start) */
int gui_win_x(int term);
int gui_win_y(int term);
int gui_cell_visible(int term, int px, int py);

void gui_status_left(const char *s);    /* spinnerul, in taskbar */
void gui_status_right(const char *s);   /* fallback text (mod negrafic) */
void gui_clock(void);                   /* ceasul RTC + terminalul activ */
void gui_refresh_taskmgr(void);         /* redeseneaza Task Manager (live) */
void gui_refresh_browser(void);         /* redeseneaza browserul cand se incarca */

/* management terminale (init le deschide la cerere) */
int  gui_terminal_free_slot(void);
int  gui_poll_term_request(void);
void gui_request_terminal(int slot);
void gui_open_terminal(int con);
void gui_close_terminal(int con);
int  gui_terminal_count(void);
int  gui_terminal_is_open(int con);

/* chemat de driverul de mouse la fiecare pachet (context de IRQ) */
void gui_pointer(int x, int y, int buttons);

/* chemat de driverul de tastatura inaintea cozii de consola: intoarce 1
 * daca tasta a fost consumata de GUI (ex. File Manager-ul focusat) */
int gui_key_intercept(char c);

/* Alt+F4: deschide/focuseaza/minimizeaza File Manager-ul */
void gui_fm_toggle(void);
/* Alt+F5: deschide/focuseaza/minimizeaza Notepad */
void gui_np_toggle(void);
/* Alt+F6: deschide/focuseaza/minimizeaza Task Manager */
void gui_tm_toggle(void);
/* Alt+F7: deschide/focuseaza/minimizeaza Browserul */
void gui_br_toggle(void);
/* Alt+F8: deschide/focuseaza/minimizeaza Setarile */
void gui_set_toggle(void);
/* Strange specificatiile hardware (CPU/GPU/disc) pentru pagina Specificatii.
 * Se cheama o data la boot (context de task, IF=1). */
void gui_sysinfo_gather(void);
/* Tasta Windows/Super: deschide/inchide meniul Start */
void gui_menu_toggle(void);
/* Alt+F9: deschide/focuseaza/minimizeaza Calculatorul */
void gui_calc_toggle(void);
/* F11: maximizeaza/restaureaza fereastra focusata */
void gui_maximize_focused(void);
