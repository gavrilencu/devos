#include "pci.h"
#include "io.h"

#define PCI_ADDR 0xCF8
#define PCI_DATA 0xCFC

static uint32_t addr(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off)
{
    return (uint32_t)(0x80000000u | ((uint32_t)bus << 16) |
                      ((uint32_t)dev << 11) | ((uint32_t)fn << 8) |
                      (off & 0xFC));
}

uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off)
{
    outl(PCI_ADDR, addr(bus, dev, fn, off));
    return inl(PCI_DATA);
}

void pci_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint32_t v)
{
    outl(PCI_ADDR, addr(bus, dev, fn, off));
    outl(PCI_DATA, v);
}

uint16_t pci_read16(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off)
{
    uint32_t v = pci_read32(bus, dev, fn, off & 0xFC);
    return (uint16_t)(v >> ((off & 2) * 8));
}

int pci_find(uint16_t vendor, uint16_t device,
             uint8_t *obus, uint8_t *odev, uint8_t *ofn)
{
    for (int bus = 0; bus < 256; bus++) {
        for (int dev = 0; dev < 32; dev++) {
            for (int fn = 0; fn < 8; fn++) {
                uint32_t id = pci_read32((uint8_t)bus, (uint8_t)dev,
                                         (uint8_t)fn, 0);
                if ((id & 0xFFFF) != vendor)
                    continue;
                if ((id >> 16) != device)
                    continue;
                *obus = (uint8_t)bus;
                *odev = (uint8_t)dev;
                *ofn = (uint8_t)fn;
                return 1;
            }
        }
    }
    return 0;
}

void pci_enable_bus_master(uint8_t bus, uint8_t dev, uint8_t fn)
{
    uint32_t cmd = pci_read32(bus, dev, fn, 0x04);
    cmd |= 0x05;   /* bit0 = I/O space, bit2 = bus master */
    pci_write32(bus, dev, fn, 0x04, cmd);
}
