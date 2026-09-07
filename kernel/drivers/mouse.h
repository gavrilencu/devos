#pragma once

/* Mouse PS/2 pe IRQ12, prin portul auxiliar al controllerului 8042.
 * Pachetele de 3 bytes (butoane + dx + dy) actualizeaza pozitia, iar
 * evenimentele ajung in GUI prin gui_pointer(). */

void mouse_init(void);
