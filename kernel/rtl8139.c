#include "net.h"
#include "pci.h"
#include "pmm.h"
#include "io.h"
#include "interrupts.h"
#include "pic.h"
#include "string.h"
#include "kprintf.h"

/* Registri RTL8139 (offset fata de baza de I/O din BAR0). */
#define REG_IDR0    0x00   /* adresa MAC (6 bytes) */
#define REG_TSD0    0x10   /* status/comanda TX, 4 descriptori la +4 */
#define REG_TSAD0   0x20   /* adresa fizica TX, 4 la +4 */
#define REG_RBSTART 0x30   /* adresa fizica a buffer-ului RX */
#define REG_CMD     0x37   /* comanda: RST(4) RE(3) TE(2) */
#define REG_CAPR    0x38   /* pozitia de citire in buffer-ul RX */
#define REG_IMR     0x3C   /* masca de intreruperi */
#define REG_ISR     0x3E   /* status intreruperi */
#define REG_TCR     0x40   /* config TX */
#define REG_RCR     0x44   /* config RX */
#define REG_CONFIG1 0x52

#define RX_BUF_SIZE (8192 + 16 + 1500)   /* 8K + antet + zona de WRAP */
#define TX_BUF_SIZE 2048

uint8_t net_mac[ETH_ALEN];

static uint16_t io_base;
static uint8_t *rx_buf;
static uint8_t *tx_buf[4];
static int tx_cur;
static uint32_t rx_off;               /* offsetul nostru in buffer-ul RX */
static int up;

static void (*rx_handler)(const uint8_t *frame, uint16_t len);

void net_set_rx(void (*fn)(const uint8_t *frame, uint16_t len))
{
    rx_handler = fn;
}

int net_up(void)
{
    return up;
}

static void handle_rx(void)
{
    /* CMD bit 0 (BUFE) = buffer gol. Cat timp avem pachete, le procesam. */
    while (!(inb(io_base + REG_CMD) & 0x01)) {
        uint8_t *p = rx_buf + rx_off;
        uint16_t status = (uint16_t)(p[0] | (p[1] << 8));
        uint16_t len = (uint16_t)(p[2] | (p[3] << 8));   /* include CRC (4) */

        if ((status & 0x01) && len >= 4 && len <= RX_BUF_SIZE) {
            if (rx_handler)
                rx_handler(p + 4, (uint16_t)(len - 4));
        }

        /* avansam la urmatorul pachet (aliniat la 4 bytes) */
        rx_off = (rx_off + len + 4 + 3) & ~3u;
        rx_off %= 8192;
        /* CAPR e "cu 16 in urma" (ciudatenie hardware) */
        outw(io_base + REG_CAPR, (uint16_t)(rx_off - 16));
    }
}

static void rtl_irq(struct int_frame *f)
{
    (void)f;
    uint16_t isr = inw(io_base + REG_ISR);
    outw(io_base + REG_ISR, isr);      /* confirmam (write-1-to-clear) */
    if (isr & 0x01)                    /* ROK: pachet primit */
        handle_rx();
}

void net_send(const void *frame, uint16_t len)
{
    if (!up || len > TX_BUF_SIZE)
        return;
    int d = tx_cur;
    tx_cur = (tx_cur + 1) & 3;

    memcpy(tx_buf[d], frame, len);
    if (len < 60)                      /* Ethernet cere minim 60 bytes */
        memset(tx_buf[d] + len, 0, 60 - len), len = 60;

    outl(io_base + REG_TSAD0 + d * 4, (uint32_t)(uint64_t)tx_buf[d]);
    /* TSD: scriind lungimea (bitii 0-12) pornim transmisia */
    outl(io_base + REG_TSD0 + d * 4, len);
}

int net_init(void)
{
    uint8_t bus, dev, fn;
    /* Realtek RTL8139: vendor 0x10EC, device 0x8139 */
    if (!pci_find(0x10EC, 0x8139, &bus, &dev, &fn)) {
        kprintf("[net] placa RTL8139 negasita\n");
        return 0;
    }
    pci_enable_bus_master(bus, dev, fn);

    /* BAR0 = baza de I/O (bitul 0 = 1 pentru spatiu de I/O) */
    uint32_t bar0 = pci_read32(bus, dev, fn, 0x10);
    io_base = (uint16_t)(bar0 & ~0x3u);

    /* linia de IRQ */
    uint8_t irq = (uint8_t)(pci_read32(bus, dev, fn, 0x3C) & 0xFF);

    /* pornire + reset */
    outb(io_base + REG_CONFIG1, 0x00);
    outb(io_base + REG_CMD, 0x10);
    while (inb(io_base + REG_CMD) & 0x10)
        ;

    /* buffere DMA (fizic = virtual, sub 256 MiB, identity-mapped) */
    rx_buf = (uint8_t *)pmm_alloc_contig((RX_BUF_SIZE + 4095) / 4096);
    for (int i = 0; i < 4; i++)
        tx_buf[i] = (uint8_t *)pmm_alloc();
    if (!rx_buf || !tx_buf[0]) {
        kprintf("[net] fara memorie pentru buffere\n");
        return 0;
    }

    outl(io_base + REG_RBSTART, (uint32_t)(uint64_t)rx_buf);
    outw(io_base + REG_IMR, 0x0005);           /* ROK + TOK */
    outl(io_base + REG_RCR, 0x8F);             /* AAP|APM|AM|AB|WRAP */
    outb(io_base + REG_CMD, 0x0C);             /* RE + TE */

    for (int i = 0; i < ETH_ALEN; i++)
        net_mac[i] = inb(io_base + REG_IDR0 + i);

    irq_install(irq, rtl_irq);
    pic_unmask(irq);

    up = 1;
    kprintf("[net] RTL8139 la I/O 0x%x, IRQ %u, MAC "
            "%02x:%02x:%02x:%02x:%02x:%02x\n",
            io_base, irq, net_mac[0], net_mac[1], net_mac[2],
            net_mac[3], net_mac[4], net_mac[5]);
    return 1;
}
