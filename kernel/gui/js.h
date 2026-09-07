#pragma once
#include <stdint.h>

/* Motor JavaScript minimal (subset ES5-ish, cu arrow functions simple) pentru
 * browserul MyOS. Tree-walking. Toata memoria (AST, obiecte, string-uri) vine
 * dintr-o arena bump, resetata la fiecare pagina. NU e V8: ruleaza JS simplu/
 * vanilla (variabile, functii, bucle, obiecte, array-uri, string-uri, DOM de
 * baza), nu framework-uri mari. */

typedef struct JS JS;

/* creeaza motorul folosind o arena data (memorie deja alocata) */
JS  *js_create(void *arena, uint32_t bytes);
void js_reset(JS *J);                 /* goleste arena + reface globalele */
int  js_eval(JS *J, const char *src, int len);   /* 0 ok, -1 eroare */
const char *js_error(JS *J);

/* Legatura cu DOM-ul: browserul furnizeaza aceste callback-uri.
 * handle = index de nod DOM (>=0), -1 = inexistent. */
struct js_dom_ops {
    void *ud;
    void (*write)(void *ud, const char *s, int len);        /* document.write */
    int  (*get_by_id)(void *ud, const char *id);            /* getElementById */
    int  (*create)(void *ud, const char *tag);              /* createElement */
    void (*append)(void *ud, int parent, int child);        /* appendChild */
    void (*set_inner)(void *ud, int h, const char *s, int len);   /* innerHTML= */
    int  (*get_inner)(void *ud, int h, char *buf, int cap);       /* innerHTML  */
    void (*set_text)(void *ud, int h, const char *s, int len);    /* textContent= */
    void (*set_attr)(void *ud, int h, const char *k, const char *v);
    void (*log)(void *ud, const char *s, int len);          /* console.log */
    int  body;                                              /* handle document.body */
};
void js_set_dom(JS *J, struct js_dom_ops *ops);
