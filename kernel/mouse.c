#include <stdint.h>
#include "mouse.h"
#include "io.h"
#include "interrupts.h"
#include "fb.h"
#include "gui.h"

#define KBC_STATUS 0x64
#define KBC_CMD    0x64
#define KBC_DATA   0x60

static int mx, my;
static uint8_t pkt[3];
static int cycle;

static void kbc_wait_in(void)      /* pana putem scrie spre controller */
{
    for (int i = 0; i < 100000; i++)
        if (!(inb(KBC_STATUS) & 0x02))
            return;
}

static void kbc_wait_out(void)     /* pana exista date de citit */
{
    for (int i = 0; i < 100000; i++)
        if (inb(KBC_STATUS) & 0x01)
            return;
}

static void mouse_cmd(uint8_t cmd)
{
    kbc_wait_in();
    outb(KBC_CMD, 0xD4);           /* urmatorul byte merge la mouse */
    kbc_wait_in();
    outb(KBC_DATA, cmd);
    kbc_wait_out();
    (void)inb(KBC_DATA);           /* ACK (0xFA) */
}

static void ms_irq(struct int_frame *f)
{
    (void)f;
    uint8_t b = inb(KBC_DATA);

    switch (cycle) {
    case 0:
        if (!(b & 0x08))           /* bitul 3 e mereu 1: resincronizare */
            return;
        pkt[0] = b;
        cycle = 1;
        break;
    case 1:
        pkt[1] = b;
        cycle = 2;
        break;
    default: {
        pkt[2] = b;
        cycle = 0;

        int dx = (int8_t)pkt[1];
        int dy = (int8_t)pkt[2];
        mx += dx;
        my -= dy;                  /* la mouse, y pozitiv = in sus */

        int w = fb_active() ? fb_width() : 640;
        int h = fb_active() ? fb_height() : 400;
        if (mx < 0) mx = 0;
        if (my < 0) my = 0;
        if (mx > w - 1) mx = w - 1;
        if (my > h - 1) my = h - 1;

        gui_pointer(mx, my, pkt[0] & 7);
        break;
    }
    }
}

void mouse_init(void)
{
    kbc_wait_in();
    outb(KBC_CMD, 0xA8);           /* activeaza portul auxiliar */

    kbc_wait_in();
    outb(KBC_CMD, 0x20);           /* citim "command byte"-ul controllerului */
    kbc_wait_out();
    uint8_t cb = inb(KBC_DATA);
    cb |= 0x02;                    /* IRQ12 activ */
    cb &= (uint8_t)~0x20;          /* ceasul mouse-ului pornit */
    kbc_wait_in();
    outb(KBC_CMD, 0x60);
    kbc_wait_in();
    outb(KBC_DATA, cb);

    mouse_cmd(0xF6);               /* setari implicite */
    mouse_cmd(0xF4);               /* porneste raportarea */

    mx = fb_active() ? fb_width() / 2 : 320;
    my = fb_active() ? fb_height() / 2 : 200;

    irq_install(12, ms_irq);
}
