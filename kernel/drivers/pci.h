#pragma once
#include <stdint.h>

/* Acces la spatiul de configurare PCI, prin porturile 0xCF8/0xCFC. */

uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off);
void     pci_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint32_t v);
uint16_t pci_read16(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off);

/* Cauta primul dispozitiv cu (vendor, device) dat. Intoarce 1 si scrie
 * bus/dev/fn, sau 0 daca nu exista. */
int pci_find(uint16_t vendor, uint16_t device,
             uint8_t *bus, uint8_t *dev, uint8_t *fn);

/* Activeaza I/O + bus mastering pentru un dispozitiv (registrul Command). */
void pci_enable_bus_master(uint8_t bus, uint8_t dev, uint8_t fn);
