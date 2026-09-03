/* Linker script pentru programele user MyOS (C, ELF static). */

OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)

SECTIONS
{
    . = 0x8000000000;

    .text   : { *(.text .text.*) }
    .rodata : { *(.rodata .rodata.*) }
    .data   : { *(.data .data.*) }
    .bss    : { *(COMMON) *(.bss .bss.*) }

    /DISCARD/ : { *(.note*) *(.comment) *(.eh_frame*) }
}
