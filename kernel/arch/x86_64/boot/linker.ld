/* MyOS — linker script pentru kernel.
 * stage2 sare la 0x9000, deci .entry (cu _start) trebuie sa fie prima. */

OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)

SECTIONS
{
    . = 0x9000;

    .entry : { *(.entry) }
    .text  : { *(.text .text.*) }
    .rodata : { *(.rodata .rodata.*) }
    .data  : { *(.data .data.*) }

    .bss : {
        __bss_start = .;
        *(COMMON)
        *(.bss .bss.*)
        __bss_end = .;
    }

    /* Sfarsitul kernelului in memorie (inclusiv .bss) — PMM rezerva tot
     * ce e sub aceasta adresa. */
    __kernel_end = .;

    /DISCARD/ : { *(.note*) *(.comment) *(.eh_frame*) }
}
