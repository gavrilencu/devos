/* Informatii despre procesor prin instructiunea CPUID + masurare TSC. */
#ifndef CPUINFO_H
#define CPUINFO_H
#include <stdint.h>

void cpu_vendor(char *s12);          /* 12 caractere + NUL (ex. GenuineIntel) */
int  cpu_brand(char *s49);           /* 48 caractere + NUL; 0 daca indisponibil */
int  cpu_logical(void);              /* nr. de procesoare logice raportate */
void cpu_features(char *s, int max); /* lista scurta: "SSE SSE2 AVX ..." */

/* Frecventa masurata (MHz), calculata la boot de cpu_info_init(); 0 daca n-a
 * fost masurata inca. */
void cpu_info_init(void);
int  cpu_mhz(void);

#endif
