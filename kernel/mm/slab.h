#pragma once
#include <stdint.h>

/* Allocator de tip slab / object cache (Milestone 55). Fiecare cache tine
 * obiecte de o dimensiune FIXA, taiate din cadre fizice (slab-uri) intr-o
 * free-list intrusiva. alloc/free sunt O(1) si nu fragmenteaza — potrivit
 * pentru structuri kernel alocate/eliberate des (ex. noduri, buffere mici). */

typedef struct slab_cache slab_cache_t;

slab_cache_t *slab_cache_create(const char *name, uint32_t obj_size);
void *slab_alloc(slab_cache_t *c);
void  slab_free(slab_cache_t *c, void *obj);

/* Statistici (pentru meminfo). */
uint32_t    slab_inuse(const slab_cache_t *c);
uint32_t    slab_slabs(const slab_cache_t *c);
uint32_t    slab_objsize(const slab_cache_t *c);
const char *slab_name(const slab_cache_t *c);
int         slab_cache_count(void);
const slab_cache_t *slab_cache_get(int i);

void slab_selftest(void);    /* verifica corectitudinea si logheaza rezultatul */
