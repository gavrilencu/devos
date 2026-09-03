#pragma once
#include <stddef.h>

/* Heap-ul kernelului: alocari de dimensiune arbitrara (kmalloc/kfree)
 * peste cadrele fizice date de PMM. Free-list ordonata dupa adresa,
 * cu spargere si coalescenta a blocurilor. */

void kheap_init(void);
void *kmalloc(size_t size);
void kfree(void *ptr);
size_t kheap_free_bytes(void);
size_t kheap_total_bytes(void);
