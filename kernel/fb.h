#pragma once
#include <stdint.h>

/* Driver de framebuffer (mod grafic VESA, 32 bpp), setat de stage2.
 * Culorile sunt 0xRRGGBB. */

int fb_init(void);       /* mapeaza framebuffer-ul; 1 = mod grafic activ */
int fb_active(void);

/* Double-buffering: desenele merg intr-un back buffer; fb_flush copiaza
 * regiunea modificata pe ecran (chemat des din IRQ-ul de timer). */
void fb_flush(void);
int fb_width(void);
int fb_height(void);

/* Schimba rezolutia la cald (prin dispi). Accepta doar dimensiuni <= cele
 * de la boot (LFB si back buffer sunt dimensionate pentru maxim).
 * Intoarce 1 la succes. */
int fb_set_mode(int w, int h);
int fb_max_width(void);
int fb_max_height(void);

void fb_putpixel(int x, int y, uint32_t rgb);
void fb_fill(int x, int y, int w, int h, uint32_t rgb);
void fb_fill_round(int x, int y, int w, int h, int r, uint32_t rgb);

/* Varianta cu anti-aliasing pe colturi. aa: 0 = margini dure,
 * 1 = AA amestecat cu o culoare solida `under` (repetabil fara degradare),
 * 2 = AA amestecat cu ce e deja pe ecran (pentru desene o singura data). */
void fb_fill_round2(int x, int y, int w, int h, int r, uint32_t rgb,
                    int aa, uint32_t under);
uint32_t fb_getpixel(int x, int y);

/* Dreptunghi de clipping: toate desenele (fill/char/pixel) sunt taiate la
 * el — compositorul il foloseste ca sa repare doar regiunea expusa. */
void fb_set_clip(int x, int y, int w, int h);
void fb_clear_clip(void);

/* Copiaza un rand de pixeli (ex. dintr-un wallpaper tinut in RAM),
 * respectand clipul. */
void fb_copy_row(int x, int y, const uint32_t *src, int n);
/* deseneaza o imagine RGBA (0xAARRGGBB) cu alpha blending (iconuri AA) */
void fb_blit_rgba(int x, int y, const uint32_t *px, int w, int h);
void fb_char(int x, int y, char ch, uint32_t fg, uint32_t bg);   /* 8x16 */
void fb_text(int x, int y, const char *s, uint32_t fg, uint32_t bg);
void fb_text_scaled(int x, int y, const char *s, uint32_t fg, int scale);

uint32_t fb_vga_color(int idx);   /* paleta celor 16 culori de text */
