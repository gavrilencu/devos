#include <stdint.h>
#include "vga.h"
#include "serial.h"
#include "kprintf.h"
#include "interrupts.h"
#include "pic.h"
#include "pit.h"
#include "keyboard.h"
#include "pmm.h"
#include "vmm.h"
#include "kheap.h"
#include "task.h"
#include "tss.h"
#include "fs.h"
#include "kheap.h"
#include "fb.h"
#include "gui.h"
#include "mouse.h"
#include "net.h"
#include "netstack.h"
#include "browser.h"
#include "shell.h"
#include "cpuinfo.h"

extern char stack_top[];   /* din entry.asm */

/* Afiseaza "[Fn] uptime: Ns" — in taskbar (mod grafic) sau in coltul
 * dreapta-sus (mod text). Ruleaza din IRQ-ul de timer, o data pe secunda. */
static void show_uptime(uint64_t secs)
{
    char buf[24];
    char *end = buf + sizeof(buf) - 1;
    char *p = end;

    *p = '\0';
    *--p = 's';
    do {
        *--p = (char)('0' + secs % 10);
        secs /= 10;
    } while (secs);

    static const char pre[] = "uptime: ";
    for (int i = (int)sizeof(pre) - 2; i >= 0; i--)
        *--p = pre[i];

    /* aratam si terminalul activ: "[F2] uptime: 42s" */
    *--p = ' ';
    *--p = ']';
    *--p = (char)('1' + console_active());
    *--p = 'F';
    *--p = '[';

    if (fb_active()) {
        gui_clock();   /* in mod grafic, taskbar-ul arata ceasul RTC real */
        gui_refresh_taskmgr();   /* Task Manager live (CPU%, stare) */
    } else {
        vga_write_at(0, 80 - (int)(end - p), p, VGA_YELLOW, VGA_BLACK);
    }
}

/* kmain — punctul de intrare in C, apelat din entry.asm.
 * Aici suntem deja in long mode (64-bit), cu primul 1 GiB identity-mapped,
 * intreruperi dezactivate si stiva proprie. */
/* Activeaza SSE (necesar pentru double in motorul JS). Doar firul browserului
 * foloseste xmm, iar restul kernelului e compilat -mno-sse, deci registrele
 * xmm nu trebuie salvate la comutarea de context. */
static void enable_sse(void)
{
    uint64_t cr0, cr4;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~(1UL << 2);          /* EM = 0 (fara emulare) */
    cr0 |=  (1UL << 1);          /* MP = 1 */
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0));
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1UL << 9) | (1UL << 10);  /* OSFXSR | OSXMMEXCPT */
    __asm__ volatile("mov %0, %%cr4" : : "r"(cr4));
}

void kmain(void)
{
    enable_sse();
    serial_init();
    console_init();

    con_color(VGA_LIGHT_CYAN, VGA_BLACK);
    kprintf("DevOS v0.49 (x86-64)\n\n");
    con_color(VGA_LIGHT_GREY, VGA_BLACK);

    gdt_init();
    tss_init((uint64_t)stack_top);
    idt_init();
    kprintf("[ok] GDT (cu selectori user + TSS) si IDT incarcate\n");

    /* Self-test: int3 declanseaza exceptia #BP; daca IDT-ul e corect,
     * handlerul afiseaza un mesaj si executia continua de aici. */
    __asm__ volatile("int3");

    pmm_init();

    /* Self-test alocator fizic: alocam trei cadre, eliberam unul si
     * verificam ca urmatoarea alocare il refoloseste (first-fit). */
    uint64_t f1 = pmm_alloc();
    uint64_t f2 = pmm_alloc();
    uint64_t f3 = pmm_alloc();
    pmm_free(f2);
    uint64_t f4 = pmm_alloc();
    kprintf("[%s] PMM: alocare/eliberare/refolosire de cadre fizice\n",
            (f4 == f2 && f1 && f3) ? "ok" : "EROARE");
    pmm_free(f1);
    pmm_free(f3);
    pmm_free(f4);

    vmm_init();

    int gfx = fb_init();
    if (gfx)
        kprintf("[ok] Mod grafic: %dx%d, 32 bpp\n", fb_width(), fb_height());

    kheap_init();
    kprintf("[ok] Heap kernel: %llu KiB\n",
            (unsigned long long)(kheap_total_bytes() / 1024));

    fs_init();

    /* splash-ul vine dupa FS: isi incarca imaginea de fundal de pe disc */
    if (gfx) {
        splash_show();
        fb_flush();               /* inca nu avem timer: prezentam manual */
    }

    pic_init();
    pit_init(100);
    keyboard_init();
    mouse_init();
    __asm__ volatile("sti");
    kprintf("[ok] Intreruperi active: PIT, tastatura (IRQ1), mouse (IRQ12)\n");

    cpu_info_init();        /* masoara frecventa CPU (are nevoie de PIT + IF=1) */
    gui_sysinfo_gather();   /* strange specificatiile hardware (CPU/GPU/disc) */

    if (net_init()) {            /* placa de retea (RTL8139) */
        net_stack_init();        /* Ethernet + ARP + IP + ICMP */
        tcp_init();              /* TCP + server telnet pe portul 23 */
    }

    sched_init();                            /* contextul curent devine task 0 */
    kprintf("[ok] Multitasking pornit: scheduler round-robin preemptiv\n");

    browser_init();                          /* browser web + firul lui de retea */

    if (gfx) {
        splash_animate();                    /* bara + spinner, ~2 secunde */
        desktop_draw(console_active());
        console_enable_render();
        gui_windows_open();                  /* nimic deschis: doar desktopul */
        gui_clock();
        gui_desktop_ready();                 /* hover + click + drag active */
        fb_flush();
    }

    pit_set_second_callback(show_uptime);
    pit_set_fast_callback(gui_refresh_browser);   /* browser live la incarcare */

    /* kmain devine "init": porneste cate un shell user pe fiecare terminal
     * virtual si il reporneste pe cel care moare (ca getty in Unix).
     * Nu mai pornim terminale automat: utilizatorul le deschide din
     * taskbar (butonul Terminal, click sau click dreapta > Terminal nou).
     * Daca nu suntem in mod grafic, cadem in shell-ul kernel. */
    kprintf("\nBun venit in DevOS!\n");

    uint32_t ush_size = 0;
    void *ush = fs_read_file("ush", &ush_size);
    if (gfx && ush) {
        int sh[CON_COUNT];              /* id-ul ush-ului de pe fiecare terminal */
        for (int t = 0; t < CON_COUNT; t++)
            sh[t] = -1;
        for (;;) {
            /* terminalele al caror shell a murit se inchid */
            for (int t = 0; t < CON_COUNT; t++)
                if (sh[t] >= 0 && !task_alive(sh[t])) {
                    sh[t] = -1;
                    gui_close_terminal(t);
                }
            /* cerere de terminal din GUI: -2 = primul liber, 0..N = anume */
            int req = gui_poll_term_request();
            if (req == -2)
                req = gui_terminal_free_slot();
            if (req >= 0 && req < CON_COUNT && sh[req] < 0) {
                sh[req] = task_create_user("ush", ush, ush_size, "", req);
                gui_open_terminal(req);
            }
            __asm__ volatile("hlt");
        }
    }

    kprintf("\nShell-ul kernel: scrie 'help' pentru comenzi.\n\n");
    shell_run();
}
