#include <stddef.h>
#include <stdint.h>
#include "shell.h"
#include "kprintf.h"
#include "keyboard.h"
#include "vga.h"
#include "pit.h"
#include "pmm.h"
#include "vmm.h"
#include "kheap.h"
#include "task.h"
#include "fs.h"
#include "kheap.h"
#include "io.h"
#include "string.h"

#define LINE_MAX 128

struct command {
    const char *name;
    const char *help;
    void (*fn)(const char *args);
};

static void cmd_help(const char *args);
static void cmd_clear(const char *args);
static void cmd_echo(const char *args);
static void cmd_mem(const char *args);
static void cmd_uptime(const char *args);
static void cmd_heap(const char *args);
static void cmd_ps(const char *args);
static void cmd_spawn(const char *args);
static void cmd_vmtest(const char *args);
static void cmd_ls(const char *args);
static void cmd_cat(const char *args);
static void cmd_run(const char *args);
static void cmd_save(const char *args);
static void cmd_rm(const char *args);
static void cmd_panic(const char *args);
static void cmd_reboot(const char *args);

static const struct command commands[] = {
    { "help",   "lista comenzilor",                        cmd_help },
    { "clear",  "curata ecranul",                          cmd_clear },
    { "echo",   "afiseaza textul dat ca argument",         cmd_echo },
    { "mem",    "starea memoriei (fizica + heap)",         cmd_mem },
    { "uptime", "de cat timp ruleaza sistemul",            cmd_uptime },
    { "heap",   "self-test kmalloc/kfree",                 cmd_heap },
    { "ps",     "lista task-urilor",                       cmd_ps },
    { "spawn",  "porneste un task demo care numara",       cmd_spawn },
    { "vmtest", "self-test mapare/demapare pagini",        cmd_vmtest },
    { "ls",     "lista fisierelor de pe disc",             cmd_ls },
    { "cat",    "afiseaza un fisier: cat <nume>",          cmd_cat },
    { "save",   "scrie un fisier: save <nume> <text>",     cmd_save },
    { "rm",     "sterge un fisier: rm <nume>",             cmd_rm },
    { "run",    "ruleaza un program in ring 3: run <nume>", cmd_run },
    { "panic",  "declanseaza intentionat un page fault",   cmd_panic },
    { "reboot", "reporneste masina",                       cmd_reboot },
};

#define NCOMMANDS (sizeof(commands) / sizeof(commands[0]))

static void cmd_help(const char *args)
{
    (void)args;
    for (size_t i = 0; i < NCOMMANDS; i++) {
        kprintf("  %s", commands[i].name);
        for (size_t p = strlen(commands[i].name); p < 8; p++)
            kputc(' ');
        kprintf("- %s\n", commands[i].help);
    }
}

static void cmd_clear(const char *args)
{
    (void)args;
    con_clear();
}

static void cmd_echo(const char *args)
{
    kprintf("%s\n", args);
}

static void cmd_mem(const char *args)
{
    (void)args;
    kprintf("memorie fizica libera: %llu KiB\n",
            (unsigned long long)(pmm_free_bytes() / 1024));
    kprintf("heap kernel:           %llu KiB liberi din %llu KiB\n",
            (unsigned long long)(kheap_free_bytes() / 1024),
            (unsigned long long)(kheap_total_bytes() / 1024));
}

static void cmd_uptime(const char *args)
{
    (void)args;
    uint64_t s = pit_ticks() / 100;
    kprintf("uptime: %llum %llus\n",
            (unsigned long long)(s / 60), (unsigned long long)(s % 60));
}

static void cmd_heap(const char *args)
{
    (void)args;
    void *a = kmalloc(100);
    void *b = kmalloc(5000);
    void *c = kmalloc(64);
    kprintf("kmalloc(100)=%p  kmalloc(5000)=%p  kmalloc(64)=%p\n", a, b, c);

    kfree(b);
    void *d = kmalloc(4000);
    kprintf("dupa kfree(5000): kmalloc(4000)=%p (%s)\n", d,
            d == b ? "a refolosit golul, corect" : "NEASTEPTAT");

    kfree(a);
    kfree(c);
    kfree(d);
    kprintf("dupa eliberarea tuturor: %llu KiB liberi in heap\n",
            (unsigned long long)(kheap_free_bytes() / 1024));
}

static void cmd_ps(const char *args)
{
    (void)args;
    task_ps();
}

/* Task demo: numara pe primul rand al ecranului, in dreptul id-ului sau. */
static void counter_thread(void)
{
    int id = task_current_id();
    int col = 2 + (id - 2) * 10;
    uint64_t n = 0;
    char buf[16];

    for (;;) {
        n++;

        int p = 0;
        buf[p++] = 'T';
        buf[p++] = (char)('0' + id % 10);
        buf[p++] = ':';
        char tmp[20];
        int i = 0;
        uint64_t v = n;
        do {
            tmp[i++] = (char)('0' + v % 10);
            v /= 10;
        } while (v);
        while (i-- && p < (int)sizeof(buf) - 1)
            buf[p++] = tmp[i];
        buf[p] = '\0';

        vga_write_at(0, col, buf, VGA_LIGHT_MAGENTA, VGA_BLACK);
        task_sleep(300);
    }
}

static void cmd_spawn(const char *args)
{
    (void)args;
    int id = task_create("demo", counter_thread);
    if (id < 0)
        kprintf("nu mai e loc in tabela de task-uri\n");
    else
        kprintf("task %d creat: numara pe primul rand al ecranului\n", id);
}

static void cmd_vmtest(const char *args)
{
    (void)args;

    uint64_t phys = pmm_alloc();
    uint64_t virt = 0x40000000;   /* prima adresa din afara identity map-ului */

    if (phys == 0 || vmm_map(virt, phys, VMM_W) < 0) {
        kprintf("vmtest: nu am putut mapa\n");
        return;
    }
    kprintf("cadrul fizic %p mapat la adresa virtuala %p\n",
            (void *)phys, (void *)virt);

    *(volatile uint64_t *)virt = 0x1234567890ABCDEFull;
    uint64_t readback = *(volatile uint64_t *)phys;
    kprintf("scris prin virtuala, citit prin fizica: 0x%llx (%s)\n",
            (unsigned long long)readback,
            readback == 0x1234567890ABCDEFull ? "identic, corect" : "DIFERIT!");

    uint64_t tr = vmm_translate(virt);
    kprintf("vmm_translate(%p) = %p (%s)\n", (void *)virt, (void *)tr,
            tr == phys ? "corect" : "GRESIT");

    vmm_unmap(virt);
    kprintf("dupa unmap: translate = %s\n",
            vmm_translate(virt) == VMM_NOT_MAPPED ? "nemapat, corect" : "INCA MAPAT!");

    pmm_free(phys);
}

static void cmd_ls(const char *args)
{
    (void)args;
    int n = fs_count();
    if (n == 0) {
        kprintf("niciun fisier (sistemul de fisiere nu e montat)\n");
        return;
    }
    for (int i = 0; i < n; i++) {
        const struct fs_file *f = fs_get(i);
        kprintf("  %s", f->name);
        for (size_t p = strlen(f->name); p < 16; p++)
            kputc(' ');
        kprintf("%u bytes\n", f->size);
    }
}

static void cmd_cat(const char *args)
{
    if (*args == '\0') {
        kprintf("utilizare: cat <nume>\n");
        return;
    }
    uint32_t size = 0;
    char *buf = fs_read_file(args, &size);
    if (!buf) {
        kprintf("nu exista fisierul '%s'\n", args);
        return;
    }
    for (uint32_t i = 0; i < size; i++)
        kputc(buf[i]);
    if (size && buf[size - 1] != '\n')
        kputc('\n');
    kfree(buf);
}

static void cmd_save(const char *args)
{
    /* despartim numele de continut */
    char name[24];
    int i = 0;
    while (args[i] && args[i] != ' ' && i < 23) {
        name[i] = args[i];
        i++;
    }
    name[i] = '\0';
    const char *text = args + i;
    while (*text == ' ')
        text++;

    if (i == 0 || *text == '\0') {
        kprintf("utilizare: save <nume> <text>\n");
        return;
    }

    uint32_t len = (uint32_t)strlen(text) + 1;   /* + newline */
    char *buf = kmalloc(len);
    if (!buf) {
        kprintf("save: memorie insuficienta\n");
        return;
    }
    memcpy(buf, text, len - 1);
    buf[len - 1] = '\n';

    int r = fs_save(name, buf, len);
    kfree(buf);

    if (r == 0)
        kprintf("scris '%s' (%u bytes) - persistent pe disc\n", name, len);
    else if (r == -2)
        kprintf("save: discul e plin\n");
    else if (r == -3)
        kprintf("save: tabelul de fisiere e plin\n");
    else
        kprintf("save: eroare de scriere\n");
}

static void cmd_rm(const char *args)
{
    if (*args == '\0') {
        kprintf("utilizare: rm <nume>\n");
        return;
    }
    int r = fs_delete(args);
    if (r == 0)
        kprintf("sters '%s'\n", args);
    else if (r == -2)
        kprintf("'%s' e fisier de sistem, protejat\n", args);
    else
        kprintf("nu exista fisierul '%s'\n", args);
}

static void cmd_run(const char *args)
{
    /* primul cuvant = numele programului; un '&' dupa el = ruleaza in fundal */
    char name[24];
    int i = 0;
    while (args[i] && args[i] != ' ' && args[i] != '&' && i < 23) {
        name[i] = args[i];
        i++;
    }
    name[i] = '\0';
    if (name[0] == '\0') {
        kprintf("utilizare: run <nume> [argumente] [&]   (vezi 'ls')\n");
        return;
    }

    /* restul liniei = argumentele programului; un '&' inseamna fundal */
    int background = 0;
    char pargs[96];
    int pi = 0;
    const char *rest = args + i;
    while (*rest == ' ')
        rest++;
    for (; *rest && pi < (int)sizeof(pargs) - 1; rest++) {
        if (*rest == '&') {
            background = 1;
            continue;
        }
        pargs[pi++] = *rest;
    }
    while (pi > 0 && pargs[pi - 1] == ' ')
        pi--;
    pargs[pi] = '\0';

    uint32_t size = 0;
    void *buf = fs_read_file(name, &size);
    if (!buf) {
        kprintf("nu exista programul '%s'\n", name);
        return;
    }

    /* task_create_user copiaza codul in paginile user, deci bufferul
     * poate fi eliberat imediat. */
    int id = task_create_user(name, buf, size, pargs, -1);
    kfree(buf);

    if (id < 0) {
        kprintf("nu am putut crea task-ul user\n");
        return;
    }

    if (background) {
        kprintf("task user %d pornit in fundal\n", id);
        return;
    }

    /* Foreground: shell-ul asteapta si nu citeste tastatura — tastele
     * ajung la program, prin syscall-ul readc. */
    while (task_alive(id))
        __asm__ volatile("hlt");
}

static void cmd_panic(const char *args)
{
    (void)args;
    kprintf("Scriu la 0x40000000 (adresa nemapata) - urmeaza #PF...\n");
    *(volatile uint64_t *)0x40000000 = 1;
}

static void cmd_reboot(const char *args)
{
    (void)args;
    kprintf("Repornesc...\n");
    outb(0x64, 0xFE);              /* linia de reset a controllerului 8042 */
    for (;;)
        __asm__ volatile("hlt");
}

static char getc_blocking(void)
{
    int c;
    while ((c = console_getchar(task_current_term())) < 0)
        __asm__ volatile("hlt");   /* dormim pana la urmatoarea intrerupere */
    return (char)c;
}

static void execute(char *line)
{
    while (*line == ' ')
        line++;
    if (*line == '\0')
        return;

    char *args = line;
    while (*args && *args != ' ')
        args++;
    if (*args) {
        *args++ = '\0';
        while (*args == ' ')
            args++;
    }

    for (size_t i = 0; i < NCOMMANDS; i++) {
        if (strcmp(line, commands[i].name) == 0) {
            commands[i].fn(args);
            return;
        }
    }
    kprintf("comanda necunoscuta: '%s' (incearca 'help')\n", line);
}

void shell_run(void)
{
    char line[LINE_MAX];

    for (;;) {
        con_color(VGA_LIGHT_GREEN, VGA_BLACK);
        kprintf("myos> ");
        con_color(VGA_LIGHT_GREY, VGA_BLACK);

        size_t pos = 0;
        for (;;) {
            char c = getc_blocking();

            if (c == '\n') {
                kputc('\n');
                break;
            }
            if (c == '\b') {
                if (pos > 0) {
                    pos--;
                    kputc('\b');
                }
                continue;
            }
            if (c == '\t')
                c = ' ';
            if (c < 32 || c > 126)
                continue;              /* ignoram ESC si alte non-printabile */

            if (pos < LINE_MAX - 1) {
                line[pos++] = c;
                kputc(c);
            }
        }
        line[pos] = '\0';
        execute(line);
    }
}
