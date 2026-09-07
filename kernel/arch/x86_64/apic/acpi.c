#include "acpi.h"
#include "vmm.h"
#include "klog.h"
#include "string.h"

/* Antet comun al tabelelor ACPI (System Description Table). */
struct sdt_header {
    char     sig[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oemid[6];
    char     oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} __attribute__((packed));

/* Root System Description Pointer (ACPI 1.0 + extensia 2.0). */
struct rsdp {
    char     sig[8];            /* "RSD PTR " */
    uint8_t  checksum;          /* suma primilor 20 octeti = 0 */
    char     oemid[6];
    uint8_t  revision;          /* 0 = ACPI 1.0 (RSDT), >=2 = 2.0 (XSDT) */
    uint32_t rsdt_addr;
    /* doar ACPI 2.0+ */
    uint32_t length;
    uint64_t xsdt_addr;
    uint8_t  ext_checksum;
    uint8_t  reserved[3];
} __attribute__((packed));

static uint32_t cpu_apic_ids[ACPI_MAX_CPUS];
static int      n_cpus;
static uint64_t lapic_phys;
static uint64_t ioapic_phys;
static uint32_t ioapic_gsi_base;

/* citiri nealiniate din intrari MADT "packed" */
static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t rd64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }

/* Asigura ca intervalul fizic e mapat (identity) ca sa-l putem citi. Tabelele
 * ACPI pot fi in zone rezervate, in afara RAM-ului identity-mapped de vmm. */
static void ensure_mapped(uint64_t phys, uint64_t len)
{
    uint64_t start = phys & ~0xFFFull;
    uint64_t end   = (phys + len + 0xFFFull) & ~0xFFFull;
    for (uint64_t p = start; p < end; p += 0x1000)
        if (vmm_translate(p) == VMM_NOT_MAPPED)
            vmm_map(p, p, VMM_W);
}

static struct rsdp *scan_rsdp(uint64_t start, uint64_t end)
{
    for (uint64_t a = start & ~0xFull; a + 20 <= end; a += 16) {
        if (memcmp((const void *)a, "RSD PTR ", 8) != 0)
            continue;
        uint8_t sum = 0;
        for (int i = 0; i < 20; i++)
            sum = (uint8_t)(sum + ((const uint8_t *)a)[i]);
        if (sum == 0)
            return (struct rsdp *)a;
    }
    return 0;
}

static struct rsdp *find_rsdp(void)
{
    /* zona de sub 1 MiB e identity-mapped, deci o putem scana direct */
    uint16_t ebda_seg = *(volatile uint16_t *)0x40E;
    uint64_t ebda = (uint64_t)ebda_seg << 4;
    struct rsdp *r = 0;
    if (ebda >= 0x400 && ebda < 0xA0000)
        r = scan_rsdp(ebda, ebda + 1024);
    if (!r)
        r = scan_rsdp(0xE0000, 0x100000);
    return r;
}

static int sig_is(const struct sdt_header *h, const char *s)
{
    return memcmp(h->sig, s, 4) == 0;
}

/* Cauta tabela MADT ("APIC") in RSDT (pointeri 32b) sau XSDT (pointeri 64b). */
static struct sdt_header *find_madt(struct rsdp *r)
{
    uint64_t root;
    int ptr_size;
    if (r->revision >= 2 && r->xsdt_addr) {
        root = r->xsdt_addr;
        ptr_size = 8;
    } else {
        root = r->rsdt_addr;
        ptr_size = 4;
    }
    ensure_mapped(root, sizeof(struct sdt_header));
    struct sdt_header *rt = (struct sdt_header *)root;
    ensure_mapped(root, rt->length);

    int count = (int)((rt->length - sizeof(struct sdt_header)) / ptr_size);
    const uint8_t *arr = (const uint8_t *)rt + sizeof(struct sdt_header);
    for (int i = 0; i < count; i++) {
        uint64_t taddr = (ptr_size == 8) ? rd64(arr + i * 8) : rd32(arr + i * 4);
        ensure_mapped(taddr, sizeof(struct sdt_header));
        struct sdt_header *h = (struct sdt_header *)taddr;
        if (sig_is(h, "APIC")) {
            ensure_mapped(taddr, h->length);
            return h;
        }
    }
    return 0;
}

static void parse_madt(struct sdt_header *m)
{
    const uint8_t *base = (const uint8_t *)m;
    lapic_phys = rd32(base + sizeof(struct sdt_header));   /* Local APIC addr */
    const uint8_t *p   = base + sizeof(struct sdt_header) + 8;
    const uint8_t *end = base + m->length;

    while (p + 2 <= end) {
        uint8_t type = p[0], len = p[1];
        if (len < 2 || p + len > end)
            break;
        switch (type) {
        case 0:                                   /* Processor Local APIC */
            if ((rd32(p + 4) & 1) && n_cpus < ACPI_MAX_CPUS)
                cpu_apic_ids[n_cpus++] = p[3];    /* APIC ID */
            break;
        case 1:                                   /* I/O APIC */
            if (!ioapic_phys) {
                ioapic_phys     = rd32(p + 4);
                ioapic_gsi_base = rd32(p + 8);
            }
            break;
        case 5:                                   /* Local APIC Address Override */
            lapic_phys = rd64(p + 4);
            break;
        default:
            break;
        }
        p += len;
    }
}

void acpi_init(void)
{
    struct rsdp *r = find_rsdp();
    if (!r) {
        KWARN("acpi", "RSDP negasit — raman pe PIC, 1 nucleu");
        n_cpus = 1;
        return;
    }
    struct sdt_header *m = find_madt(r);
    if (!m) {
        KWARN("acpi", "MADT (APIC) negasit — 1 nucleu");
        n_cpus = 1;
        return;
    }
    parse_madt(m);
    if (n_cpus == 0)
        n_cpus = 1;

    KINFO("acpi", "%d nucleu(e) CPU, Local APIC @ %p, I/O APIC @ %p (GSI %u)",
          n_cpus, (void *)lapic_phys, (void *)ioapic_phys,
          (unsigned)ioapic_gsi_base);
}

int      acpi_cpu_count(void)        { return n_cpus ? n_cpus : 1; }
uint32_t acpi_cpu_apic_id(int i)     { return (i >= 0 && i < n_cpus) ? cpu_apic_ids[i] : 0; }
uint64_t acpi_lapic_base(void)       { return lapic_phys; }
uint64_t acpi_ioapic_base(void)      { return ioapic_phys; }
uint32_t acpi_ioapic_gsi_base(void)  { return ioapic_gsi_base; }
