#include "slab.h"
#include "pmm.h"
#include "kheap.h"
#include "string.h"
#include "klog.h"

struct slab_cache {
    char     name[16];
    uint32_t obj_size;      /* dimensiunea unui obiect (aliniata la 16) */
    uint32_t per_slab;      /* obiecte pe slab (un cadru de 4 KiB) */
    void    *free_list;     /* sloturi libere (lista intrusiva) */
    uint32_t inuse;         /* obiecte alocate acum */
    uint32_t nslabs;        /* cadre folosite de acest cache */
};

#define MAX_CACHES 16
static slab_cache_t *caches[MAX_CACHES];
static int ncaches;

slab_cache_t *slab_cache_create(const char *name, uint32_t obj_size)
{
    if (ncaches >= MAX_CACHES)
        return 0;
    if (obj_size < 16)
        obj_size = 16;
    obj_size = (obj_size + 15u) & ~15u;         /* aliniere la 16 octeti */
    if (obj_size > PMM_FRAME_SIZE)
        return 0;

    slab_cache_t *c = kmalloc(sizeof(*c));
    if (!c)
        return 0;
    memset(c, 0, sizeof(*c));
    int i = 0;
    for (; name[i] && i < 15; i++)
        c->name[i] = name[i];
    c->name[i] = 0;
    c->obj_size = obj_size;
    c->per_slab = (uint32_t)(PMM_FRAME_SIZE / obj_size);
    caches[ncaches++] = c;
    return c;
}

/* Aloca un cadru nou si il taie in sloturi legate in free-list. */
static int slab_grow(slab_cache_t *c)
{
    uint64_t frame = pmm_alloc();
    if (frame == 0)
        return -1;
    uint8_t *p = (uint8_t *)frame;
    for (uint32_t i = 0; i < c->per_slab; i++) {
        void *slot = p + (uint64_t)i * c->obj_size;
        *(void **)slot = c->free_list;
        c->free_list = slot;
    }
    c->nslabs++;
    return 0;
}

void *slab_alloc(slab_cache_t *c)
{
    if (!c)
        return 0;
    if (!c->free_list && slab_grow(c) < 0)
        return 0;
    void *obj = c->free_list;
    c->free_list = *(void **)obj;
    c->inuse++;
    return obj;
}

void slab_free(slab_cache_t *c, void *obj)
{
    if (!c || !obj)
        return;
    *(void **)obj = c->free_list;
    c->free_list = obj;
    if (c->inuse)
        c->inuse--;
}

uint32_t    slab_inuse(const slab_cache_t *c)   { return c ? c->inuse : 0; }
uint32_t    slab_slabs(const slab_cache_t *c)   { return c ? c->nslabs : 0; }
uint32_t    slab_objsize(const slab_cache_t *c) { return c ? c->obj_size : 0; }
const char *slab_name(const slab_cache_t *c)    { return c ? c->name : ""; }
int         slab_cache_count(void)              { return ncaches; }
const slab_cache_t *slab_cache_get(int i)
{
    return (i >= 0 && i < ncaches) ? caches[i] : 0;
}

void slab_selftest(void)
{
    slab_cache_t *c = slab_cache_create("test64", 64);
    if (!c) {
        KERR("slab", "selftest: crearea cache-ului a esuat");
        return;
    }
    static void *p[200];
    for (int i = 0; i < 200; i++) {
        p[i] = slab_alloc(c);
        if (!p[i]) {
            KERR("slab", "selftest: alocarea %d a esuat", i);
            return;
        }
    }
    for (int i = 0; i < 200; i++)
        *(int *)p[i] = i * 7 + 1;               /* scriem in fiecare slot */
    int ok = 1;
    for (int i = 0; i < 200; i++)
        if (*(int *)p[i] != i * 7 + 1)          /* sloturile nu se suprapun? */
            ok = 0;
    for (int i = 0; i < 200; i++)
        slab_free(c, p[i]);
    KINFO("slab", "selftest: 200 obiecte x 64B in %u slaburi, integritate %s",
          (unsigned)slab_slabs(c), ok ? "OK" : "ESUAT");
}
