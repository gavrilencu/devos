#include <stdint.h>
#include "syscall.h"
#include "task.h"
#include "keyboard.h"
#include "pit.h"
#include "fs.h"
#include "kheap.h"
#include "vga.h"
#include "pmm.h"
#include "pipe.h"
#include "netstack.h"
#include "ssh.h"
#include "io.h"
#include "string.h"
#include "kprintf.h"

#define SYS_WRITE   0
#define SYS_EXIT    1
#define SYS_GETPID  2
#define SYS_SLEEP   3
#define SYS_READC   4
#define SYS_TICKS   5
#define SYS_FREAD   6
#define SYS_FWRITE  7
#define SYS_SPAWN   8
#define SYS_ALIVE   9
#define SYS_FLIST   10
#define SYS_FDELETE 11
#define SYS_CLEAR   12
#define SYS_PSLIST  13
#define SYS_MEMINFO 14
#define SYS_PIPE    15
#define SYS_SPAWN2  16
#define SYS_RTC     17
#define SYS_PING     18
#define SYS_PING_RES 19
#define SYS_TCP_CONNECT 20
#define SYS_TCP_STATUS  21
#define SYS_TCP_SEND    22
#define SYS_TCP_RECV    23
#define SYS_TCP_CLOSE   24
#define SYS_DNS         25
#define SYS_DNS_RES     26
#define SYS_SSH_OPEN    27
#define SYS_SSH_STATUS  28
#define SYS_SSH_READ    29
#define SYS_SSH_WRITE   30
#define SYS_SSH_CLOSE   31
#define SYS_SSH_ERROR   32
#define SYS_SSH_KEYGEN  33
#define SYS_SSH_PUBKEY  34
#define SYS_TERM_APPCUR 35

#define FWRITE_MAX (512 * 1024)

/* Un interval [ptr, ptr+len) e valid doar daca sta integral in user space. */
static int user_range(uint64_t ptr, uint64_t len)
{
    return ptr >= USER_CODE_BASE && ptr + len <= USER_STACK_TOP &&
           ptr + len >= ptr;
}

/* Copiaza un string NUL-terminat din user space, cu limita. */
static int copy_str(uint64_t uptr, char *dst, int cap)
{
    for (int i = 0; i < cap; i++) {
        if (!user_range(uptr + (uint64_t)i, 1))
            return -1;
        dst[i] = *(const char *)(uptr + i);
        if (dst[i] == '\0')
            return 0;
    }
    return -1;   /* prea lung sau neterminat */
}

static int copy_name(uint64_t uptr, char dst[24])
{
    if (copy_str(uptr, dst, 24) < 0 || dst[0] == '\0')
        return -1;
    return 0;
}

uint64_t syscall_handler(struct int_frame *f)
{
    switch (f->rax) {

    case SYS_WRITE: {
        uint64_t ptr = f->rdi;
        uint64_t len = f->rsi;
        /* Nu-i permitem user-ului sa ne puna sa citim memoria kernelului. */
        if (len > 4096 || ptr < USER_CODE_BASE || ptr + len > USER_STACK_TOP) {
            f->rax = (uint64_t)-1;
            return (uint64_t)f;
        }
        const char *s = (const char *)ptr;

        /* stdout redirectat intr-un pipe? */
        int op = task_current_out_pipe();
        if (op >= 0) {
            f->rax = (uint64_t)pipe_write(op, s, len);
            return (uint64_t)f;
        }

        for (uint64_t i = 0; i < len; i++)
            kputc(s[i]);
        f->rax = len;
        return (uint64_t)f;
    }

    case SYS_EXIT:
        task_kill_current();
        return sched_tick((uint64_t)f);    /* nu ne mai intoarcem in task */

    case SYS_GETPID:
        f->rax = (uint64_t)task_current_id();
        return (uint64_t)f;

    case SYS_SLEEP:
        task_sleep_current(f->rdi);
        return sched_tick((uint64_t)f);

    case SYS_READC: {
        /* stdin redirectat dintr-un pipe? (-2 = EOF) */
        int ip = task_current_in_pipe();
        if (ip >= 0)
            f->rax = (uint64_t)(int64_t)pipe_read(ip);
        else
            f->rax = (uint64_t)(int64_t)console_getchar(task_current_term());
        return (uint64_t)f;
    }

    case SYS_TICKS:
        f->rax = pit_ticks();
        return (uint64_t)f;

    case SYS_FREAD: {
        /* Syscall-ul ruleaza cu CR3-ul procesului, deci memoria user e
         * direct accesibila dupa validare. */
        char name[24];
        uint64_t ubuf = f->rsi, maxlen = f->rdx;
        if (copy_name(f->rdi, name) < 0 || !user_range(ubuf, maxlen)) {
            f->rax = (uint64_t)-1;
            return (uint64_t)f;
        }
        uint32_t size = 0;
        void *data = fs_read_file(name, &size);
        if (!data) {
            f->rax = (uint64_t)-1;
            return (uint64_t)f;
        }
        uint64_t n = size < maxlen ? size : maxlen;
        memcpy((void *)ubuf, data, n);
        kfree(data);
        f->rax = n;
        return (uint64_t)f;
    }

    case SYS_FWRITE: {
        char name[24];
        uint64_t ubuf = f->rsi, len = f->rdx;
        if (copy_name(f->rdi, name) < 0 || len == 0 || len > FWRITE_MAX ||
            !user_range(ubuf, len)) {
            f->rax = (uint64_t)-1;
            return (uint64_t)f;
        }
        f->rax = (uint64_t)(int64_t)fs_save(name, (const void *)ubuf,
                                            (uint32_t)len);
        return (uint64_t)f;
    }

    case SYS_SPAWN: {
        char name[24], args[96];
        if (copy_name(f->rdi, name) < 0 || copy_str(f->rsi, args, 96) < 0) {
            f->rax = (uint64_t)-1;
            return (uint64_t)f;
        }
        uint32_t size = 0;
        void *data = fs_read_file(name, &size);
        if (!data) {
            f->rax = (uint64_t)-1;
            return (uint64_t)f;
        }
        int id = task_create_user(name, data, size, args, -1);
        kfree(data);
        f->rax = (uint64_t)(int64_t)id;
        return (uint64_t)f;
    }

    case SYS_ALIVE:
        f->rax = (uint64_t)task_alive((int)f->rdi);
        return (uint64_t)f;

    case SYS_FLIST: {
        uint64_t ubuf = f->rdi, maxlen = f->rsi;
        if (maxlen == 0 || !user_range(ubuf, maxlen)) {
            f->rax = (uint64_t)-1;
            return (uint64_t)f;
        }
        char *out = (char *)ubuf;
        uint64_t pos = 0;
        for (int i = 0; i < fs_count(); i++) {
            const struct fs_file *fl = fs_get(i);
            for (const char *p = fl->name; *p && pos < maxlen; p++)
                out[pos++] = *p;
            for (uint64_t k = strlen(fl->name); k < 14 && pos < maxlen; k++)
                out[pos++] = ' ';
            char tmp[12];
            int t = 0;
            uint32_t v = fl->size;
            do {
                tmp[t++] = (char)('0' + v % 10);
                v /= 10;
            } while (v);
            while (t-- > 0 && pos < maxlen)
                out[pos++] = tmp[t];
            if (pos < maxlen)
                out[pos++] = '\n';
        }
        f->rax = pos;
        return (uint64_t)f;
    }

    case SYS_FDELETE: {
        char name[24];
        if (copy_name(f->rdi, name) < 0) {
            f->rax = (uint64_t)-1;
            return (uint64_t)f;
        }
        f->rax = (uint64_t)(int64_t)fs_delete(name);
        return (uint64_t)f;
    }

    case SYS_CLEAR:
        con_clear();
        f->rax = 0;
        return (uint64_t)f;

    case SYS_PSLIST: {
        uint64_t ubuf = f->rdi, maxlen = f->rsi;
        if (maxlen == 0 || !user_range(ubuf, maxlen)) {
            f->rax = (uint64_t)-1;
            return (uint64_t)f;
        }
        if (maxlen > 4096)
            maxlen = 4096;
        f->rax = (uint64_t)task_ps_dump((char *)ubuf, (int)maxlen);
        return (uint64_t)f;
    }

    case SYS_MEMINFO:
        f->rax = pmm_free_bytes();
        return (uint64_t)f;

    case SYS_PIPE:
        f->rax = (uint64_t)(int64_t)pipe_alloc();
        return (uint64_t)f;

    case SYS_PING:
        icmp_ping((uint32_t)f->rdi);
        f->rax = 0;
        return (uint64_t)f;

    case SYS_PING_RES:
        f->rax = (uint64_t)(int64_t)icmp_ping_result();
        return (uint64_t)f;

    case SYS_TCP_CONNECT:
        f->rax = (uint64_t)(int64_t)tcp_connect((uint32_t)f->rdi,
                                                (uint16_t)f->rsi);
        return (uint64_t)f;

    case SYS_TCP_STATUS:
        f->rax = (uint64_t)(int64_t)tcp_status((int)f->rdi);
        return (uint64_t)f;

    case SYS_TCP_SEND: {
        uint64_t ubuf = f->rsi, len = f->rdx;
        if (len > 4096 || !user_range(ubuf, len)) {
            f->rax = (uint64_t)-1;
            return (uint64_t)f;
        }
        f->rax = (uint64_t)(int64_t)tcp_csend((int)f->rdi,
                                              (const void *)ubuf, (int)len);
        return (uint64_t)f;
    }

    case SYS_TCP_RECV: {
        uint64_t ubuf = f->rsi, max = f->rdx;
        if (max == 0 || max > 4096 || !user_range(ubuf, max)) {
            f->rax = (uint64_t)-1;
            return (uint64_t)f;
        }
        f->rax = (uint64_t)(int64_t)tcp_crecv((int)f->rdi,
                                              (void *)ubuf, (int)max);
        return (uint64_t)f;
    }

    case SYS_TCP_CLOSE:
        tcp_cclose((int)f->rdi);
        f->rax = 0;
        return (uint64_t)f;

    case SYS_DNS: {
        char name[128];
        if (copy_str(f->rdi, name, sizeof(name)) < 0 || name[0] == '\0') {
            f->rax = (uint64_t)-1;
            return (uint64_t)f;
        }
        dns_query(name);
        f->rax = 0;
        return (uint64_t)f;
    }

    case SYS_DNS_RES: {
        uint32_t r = dns_result();
        if (r == (uint32_t)-1)
            f->rax = (uint64_t)(int64_t)-1;   /* inca astept */
        else
            f->rax = (uint64_t)r;             /* 0=esec, altfel IP */
        return (uint64_t)f;
    }

    case SYS_RTC: {
        /* ceasul hardware (CMOS), in BCD: il intoarcem ca (hh<<16)|(mm<<8)|ss */
        uint8_t v[3];
        static const uint8_t reg[3] = { 4, 2, 0 };   /* ore, minute, secunde */
        for (int i = 0; i < 3; i++) {
            outb(0x70, reg[i]);
            uint8_t b = inb(0x71);
            v[i] = (uint8_t)((b >> 4) * 10 + (b & 0x0F));
        }
        f->rax = ((uint64_t)v[0] << 16) | ((uint64_t)v[1] << 8) | v[2];
        return (uint64_t)f;
    }

    case SYS_SPAWN2: {
        char name[24], args[96];
        int inp  = (int)(int64_t)f->rdx;
        int outp = (int)(int64_t)f->rcx;
        if (copy_name(f->rdi, name) < 0 || copy_str(f->rsi, args, 96) < 0 ||
            (inp >= 0 && !pipe_valid(inp)) ||
            (outp >= 0 && !pipe_valid(outp))) {
            f->rax = (uint64_t)-1;
            return (uint64_t)f;
        }
        uint32_t size = 0;
        void *data = fs_read_file(name, &size);
        if (!data) {
            f->rax = (uint64_t)-1;
            return (uint64_t)f;
        }
        int id = task_create_user(name, data, size, args, -1);
        kfree(data);
        if (id >= 0)
            task_set_pipes(id, inp, outp);
        f->rax = (uint64_t)(int64_t)id;
        return (uint64_t)f;
    }

    case SYS_SSH_OPEN: {
        char user[64], pass[64];
        if (copy_str(f->rdx, user, sizeof(user)) < 0 ||
            copy_str(f->rcx, pass, sizeof(pass)) < 0) {
            f->rax = (uint64_t)-1;
            return (uint64_t)f;
        }
        f->rax = (uint64_t)(int64_t)ssh_open((uint32_t)f->rdi,
                                             (uint16_t)f->rsi, user, pass);
        return (uint64_t)f;
    }

    case SYS_SSH_STATUS:
        f->rax = (uint64_t)(int64_t)ssh_status();
        return (uint64_t)f;

    case SYS_SSH_READ: {
        uint64_t ubuf = f->rdi, max = f->rsi;
        if (max == 0 || max > 4096 || !user_range(ubuf, max)) {
            f->rax = (uint64_t)-1;
            return (uint64_t)f;
        }
        f->rax = (uint64_t)(int64_t)ssh_read((void *)ubuf, (int)max);
        return (uint64_t)f;
    }

    case SYS_SSH_WRITE: {
        uint64_t ubuf = f->rdi, len = f->rsi;
        if (len > 4096 || !user_range(ubuf, len)) {
            f->rax = (uint64_t)-1;
            return (uint64_t)f;
        }
        f->rax = (uint64_t)(int64_t)ssh_write((const void *)ubuf, (int)len);
        return (uint64_t)f;
    }

    case SYS_SSH_CLOSE:
        ssh_close();
        f->rax = 0;
        return (uint64_t)f;

    case SYS_SSH_ERROR: {
        uint64_t ubuf = f->rdi, max = f->rsi;
        if (max == 0 || max > 4096 || !user_range(ubuf, max)) {
            f->rax = (uint64_t)-1;
            return (uint64_t)f;
        }
        const char *e = ssh_error();
        char *d = (char *)ubuf;
        uint64_t n = 0;
        while (e && e[n] && n < max - 1) { d[n] = e[n]; n++; }
        d[n] = 0;
        f->rax = n;
        return (uint64_t)f;
    }

    case SYS_SSH_KEYGEN:
        f->rax = (uint64_t)(int64_t)ssh_keygen();
        return (uint64_t)f;

    case SYS_SSH_PUBKEY: {
        uint64_t ubuf = f->rdi, max = f->rsi;
        if (max == 0 || max > 4096 || !user_range(ubuf, max)) {
            f->rax = (uint64_t)-1;
            return (uint64_t)f;
        }
        f->rax = (uint64_t)(int64_t)ssh_get_pubkey_line((char *)ubuf, (int)max);
        return (uint64_t)f;
    }

    case SYS_TERM_APPCUR:
        f->rax = (uint64_t)(int64_t)console_app_cursor(task_current_term());
        return (uint64_t)f;

    default:
        f->rax = (uint64_t)-1;
        return (uint64_t)f;
    }
}
