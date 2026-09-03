#include "pipe.h"

#define NPIPES   8
#define PIPE_BUF 4096

struct pipe {
    char buf[PIPE_BUF];
    volatile uint32_t head, tail;
    int reader;    /* id-ul task-ului cititor, -1 daca a murit/lipseste */
    int writer;
    int used;
};

static struct pipe pipes[NPIPES];

int pipe_alloc(void)
{
    for (int i = 0; i < NPIPES; i++) {
        if (!pipes[i].used) {
            pipes[i].head = 0;
            pipes[i].tail = 0;
            pipes[i].reader = -1;
            pipes[i].writer = -1;
            pipes[i].used = 1;
            return i;
        }
    }
    return -1;
}

int pipe_valid(int p)
{
    return p >= 0 && p < NPIPES && pipes[p].used;
}

void pipe_set_reader(int p, int task_id)
{
    if (pipe_valid(p))
        pipes[p].reader = task_id;
}

void pipe_set_writer(int p, int task_id)
{
    if (pipe_valid(p))
        pipes[p].writer = task_id;
}

/* Pipe-ul dispare cand ambele capete au murit. */
static void maybe_free(int p)
{
    if (pipes[p].reader == -1 && pipes[p].writer == -1)
        pipes[p].used = 0;
}

void pipe_close_reader(int p)
{
    if (!pipe_valid(p))
        return;
    pipes[p].reader = -1;
    maybe_free(p);
}

void pipe_close_writer(int p)
{
    if (!pipe_valid(p))
        return;
    pipes[p].writer = -1;
    maybe_free(p);
}

int64_t pipe_write(int p, const char *s, uint64_t len)
{
    if (!pipe_valid(p))
        return -2;
    if (pipes[p].reader == -1)
        return -2;           /* nimeni nu mai citeste: scrisul e degeaba */

    uint64_t pushed = 0;
    while (pushed < len) {
        uint32_t next = (pipes[p].head + 1) % PIPE_BUF;
        if (next == pipes[p].tail)
            break;           /* plin: scriitorul reincearca mai tarziu */
        pipes[p].buf[pipes[p].head] = s[pushed++];
        pipes[p].head = next;
    }
    return (int64_t)pushed;
}

int pipe_read(int p)
{
    if (!pipe_valid(p))
        return -2;
    if (pipes[p].head == pipes[p].tail)
        return pipes[p].writer == -1 ? -2 : -1;
    char c = pipes[p].buf[pipes[p].tail];
    pipes[p].tail = (pipes[p].tail + 1) % PIPE_BUF;
    return (unsigned char)c;
}
