#pragma once
#include <stdint.h>

/* Task State Segment: in long mode serveste aproape exclusiv la a-i spune
 * CPU-ului ce stiva de kernel (RSP0) sa foloseasca atunci cand o
 * intrerupere/exceptie soseste in timp ce ruleaza cod in ring 3. */

void tss_init(uint64_t rsp0);          /* completeaza descriptorul din GDT + ltr */
void tss_set_rsp0(uint64_t rsp0);      /* schedulerul o cheama la comutare */
