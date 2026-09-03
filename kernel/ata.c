#include "ata.h"
#include "io.h"

/* Porturile canalului ATA primar. */
#define ATA_DATA    0x1F0
#define ATA_SECCNT  0x1F2
#define ATA_LBA_LO  0x1F3
#define ATA_LBA_MID 0x1F4
#define ATA_LBA_HI  0x1F5
#define ATA_DRIVE   0x1F6
#define ATA_STATUS  0x1F7   /* la citire */
#define ATA_CMD     0x1F7   /* la scriere */

#define ST_ERR 0x01
#define ST_DRQ 0x08
#define ST_BSY 0x80

#define CMD_READ_SECTORS  0x20
#define CMD_WRITE_SECTORS 0x30
#define CMD_FLUSH_CACHE   0xE7

static void wait_not_busy(void)
{
    while (inb(ATA_STATUS) & ST_BSY)
        ;
}

int ata_read(uint32_t lba, uint32_t count, void *buf)
{
    uint8_t *p = buf;

    while (count--) {
        wait_not_busy();

        outb(ATA_DRIVE, 0xE0 | ((lba >> 24) & 0x0F));   /* master, LBA */
        outb(ATA_SECCNT, 1);
        outb(ATA_LBA_LO,  lba & 0xFF);
        outb(ATA_LBA_MID, (lba >> 8) & 0xFF);
        outb(ATA_LBA_HI,  (lba >> 16) & 0xFF);
        outb(ATA_CMD, CMD_READ_SECTORS);

        /* asteptam datele (DRQ), cu ochii pe bitul de eroare */
        for (;;) {
            uint8_t st = inb(ATA_STATUS);
            if (st & ST_ERR)
                return -1;
            if (!(st & ST_BSY) && (st & ST_DRQ))
                break;
        }

        insw(ATA_DATA, p, 256);    /* 256 de cuvinte = 512 bytes */
        p += 512;
        lba++;
    }
    return 0;
}

int ata_write(uint32_t lba, uint32_t count, const void *buf)
{
    const uint8_t *p = buf;

    while (count--) {
        wait_not_busy();

        outb(ATA_DRIVE, 0xE0 | ((lba >> 24) & 0x0F));
        outb(ATA_SECCNT, 1);
        outb(ATA_LBA_LO,  lba & 0xFF);
        outb(ATA_LBA_MID, (lba >> 8) & 0xFF);
        outb(ATA_LBA_HI,  (lba >> 16) & 0xFF);
        outb(ATA_CMD, CMD_WRITE_SECTORS);

        for (;;) {
            uint8_t st = inb(ATA_STATUS);
            if (st & ST_ERR)
                return -1;
            if (!(st & ST_BSY) && (st & ST_DRQ))
                break;
        }

        outsw(ATA_DATA, p, 256);
        p += 512;
        lba++;

        /* discul sa scrie efectiv, nu doar in cache-ul lui */
        outb(ATA_CMD, CMD_FLUSH_CACHE);
        wait_not_busy();
    }
    return 0;
}
