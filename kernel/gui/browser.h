#pragma once

/* Browser web MyOS: descarca pagini HTTP (client TCP din kernel), le
 * parseaza si le afiseaza intr-o fereastra grafica, cu bara de adresa,
 * linkuri clicabile, istoric si derulare. TLS/HTTPS urmeaza. */

void browser_init(void);                  /* aloca buffere + porneste firul de retea */
void browser_navigate(const char *url);   /* incarca un URL (din bara sau un link) */

/* desenare + interactiune, chemate de gui.c cu originea continutului ferestrei */
void browser_draw(int cx, int cy);
void browser_set_size(int w, int h);      /* redimensioneaza continutul (maximizare) */
void browser_click(int cx, int cy, int mx, int my);
int  browser_key(char ch);                /* 1 = tasta consumata */
void browser_scroll(int lines);           /* +jos / -sus */

int  browser_poll_dirty(void);            /* 1 daca trebuie redesenat (o data) */
int  browser_is_loading(void);
const char *browser_title(void);          /* titlul paginii (pentru bara ferestrei) */
