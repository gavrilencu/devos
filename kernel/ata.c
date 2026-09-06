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
#define CMD_IDENTIFY      0xEC

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

int ata_identify(char *model, uint64_t *sectors)
{
    wait_not_busy();
    outb(ATA_DRIVE, 0xE0);              /* master */
    outb(ATA_SECCNT, 0);
    outb(ATA_LBA_LO, 0);
    outb(ATA_LBA_MID, 0);
    outb(ATA_LBA_HI, 0);
    outb(ATA_CMD, CMD_IDENTIFY);

    if (inb(ATA_STATUS) == 0)
        return -1;                      /* niciun disc */
    while (inb(ATA_STATUS) & ST_BSY)
        ;
    if (inb(ATA_LBA_MID) != 0 || inb(ATA_LBA_HI) != 0)
        return -1;                      /* nu e ATA (ATAPI/SATA)? */
    for (;;) {
        uint8_t st = inb(ATA_STATUS);
        if (st & ST_ERR)
            return -1;
        if (st & ST_DRQ)
            break;
    }

    uint16_t id[256];
    insw(ATA_DATA, id, 256);

    if (model) {                        /* cuvintele 27..46 = model, ASCII swap */
        for (int i = 0; i < 20; i++) {
            model[i * 2]     = (char)(id[27 + i] >> 8);
            model[i * 2 + 1] = (char)(id[27 + i] & 0xFF);
        }
        model[40] = 0;
        for (int i = 39; i >= 0 && (model[i] == ' ' || model[i] == 0); i--)
            model[i] = 0;
    }
    if (sectors) {
        uint64_t s = 0;
        if (id[83] & 0x400)             /* LBA48 suportat */
            s = (uint64_t)id[100] | ((uint64_t)id[101] << 16) |
                ((uint64_t)id[102] << 32) | ((uint64_t)id[103] << 48);
        if (s == 0)                     /* altfel LBA28 */
            s = (uint64_t)id[60] | ((uint64_t)id[61] << 16);
        *sectors = s;
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
