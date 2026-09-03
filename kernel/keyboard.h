#pragma once

/* Tastatura PS/2 pe IRQ1 (scancode set 1, layout US).
 * Caracterele ajung in coada terminalului ACTIV (vezi vga.h); Alt+F1..F3
 * comuta terminalul. Consumatorii citesc cu console_getchar(term). */

void keyboard_init(void);
