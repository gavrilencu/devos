#pragma once
#include <stdint.h>

/* ACPI + MADT (Milestone 56): gaseste tabelele BIOS-ului si enumereaza
 * nucleele CPU (Local APIC), I/O APIC-ul si adresa Local APIC. Nu porneste
 * inca nucleele (asta e SMP, Milestone 57) — doar descopera hardware-ul. */

#define ACPI_MAX_CPUS 32

void     acpi_init(void);
int      acpi_cpu_count(void);          /* nr. de nuclee gasite (min. 1) */
uint32_t acpi_cpu_apic_id(int i);       /* APIC ID-ul nucleului i */
uint64_t acpi_lapic_base(void);         /* adresa fizica Local APIC (MMIO) */
uint64_t acpi_ioapic_base(void);        /* adresa fizica I/O APIC (0 daca nu) */
uint32_t acpi_ioapic_gsi_base(void);    /* GSI-ul de baza al I/O APIC */
