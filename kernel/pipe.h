#pragma once
#include <stdint.h>

/* Pipe-uri unidirectionale intre procese: un buffer circular in kernel cu
 * un scriitor si un cititor. write-ul scriitorului intra in pipe (partial
 * daca e plin), readc-ul cititorului scoate din pipe; cand scriitorul a
 * murit si bufferul e gol, cititorul primeste EOF. */

int pipe_alloc(void);                 /* id-ul pipe-ului sau -1 */
int pipe_valid(int p);

void pipe_set_reader(int p, int task_id);
void pipe_set_writer(int p, int task_id);
void pipe_close_reader(int p);        /* chemate de reaper la moartea task-ului */
void pipe_close_writer(int p);

/* Scrie pana la `len` bytes; intoarce cati au incaput (0 = plin, mai
 * incearca) sau -2 daca cititorul a murit (nu mai are rost). */
int64_t pipe_write(int p, const char *s, uint64_t len);

/* Un byte, sau -1 = gol (mai incearca), -2 = EOF (scriitorul a murit). */
int pipe_read(int p);
