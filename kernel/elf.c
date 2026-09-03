#include "elf.h"
#include "pmm.h"
#include "task.h"
#include "string.h"

struct elf64_ehdr {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} __attribute__((packed));

struct elf64_phdr {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} __attribute__((packed));

#define PT_LOAD    1
#define EM_X86_64  62
#define ET_EXEC    2

/* Programul are voie sa ocupe cel mult 4 MiB sub zona de stiva. */
#define USER_CODE_LIMIT (USER_CODE_BASE + 0x400000ull)

int elf_load(address_space_t space, const void *image, uint64_t size,
             uint64_t *entry_out)
{
    const uint8_t *img = image;

    if (size < sizeof(struct elf64_ehdr))
        return -1;
    const struct elf64_ehdr *eh = (const struct elf64_ehdr *)img;

    if (eh->e_ident[0] != 0x7F || eh->e_ident[1] != 'E' ||
        eh->e_ident[2] != 'L'  || eh->e_ident[3] != 'F')
        return -1;
    if (eh->e_ident[4] != 2)                    /* 64-bit */
        return -1;
    if (eh->e_machine != EM_X86_64 || eh->e_type != ET_EXEC)
        return -1;
    if (eh->e_phoff + (uint64_t)eh->e_phnum * sizeof(struct elf64_phdr) > size)
        return -1;

    const struct elf64_phdr *ph = (const struct elf64_phdr *)(img + eh->e_phoff);

    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD)
            continue;

        uint64_t va = ph[i].p_vaddr;
        if (va < USER_CODE_BASE || va + ph[i].p_memsz > USER_CODE_LIMIT)
            return -1;
        if (ph[i].p_offset + ph[i].p_filesz > size)
            return -1;

        /* Alocam si mapam paginile care acopera [vaddr, vaddr+memsz);
         * o pagina poate fi deja mapata de un segment anterior. */
        uint64_t start = va & ~0xFFFull;
        uint64_t end   = (va + ph[i].p_memsz + 0xFFF) & ~0xFFFull;
        for (uint64_t page = start; page < end; page += PMM_FRAME_SIZE) {
            if (vmm_translate_in(space, page) != VMM_NOT_MAPPED)
                continue;
            uint64_t frame = pmm_alloc();
            if (frame == 0)
                return -1;
            memset((void *)frame, 0, PMM_FRAME_SIZE);
            if (vmm_map_in(space, page, frame, VMM_W | VMM_U) < 0)
                return -1;
        }

        /* Copiem continutul din fisier (filesz); restul pana la memsz
         * ramane zero (.bss). Mergem pagina cu pagina prin traducere. */
        for (uint64_t off = 0; off < ph[i].p_filesz; ) {
            uint64_t v = va + off;
            uint64_t phys = vmm_translate_in(space, v);
            uint64_t chunk = PMM_FRAME_SIZE - (v & 0xFFF);
            if (chunk > ph[i].p_filesz - off)
                chunk = ph[i].p_filesz - off;
            memcpy((void *)phys, img + ph[i].p_offset + off, chunk);
            off += chunk;
        }
    }

    *entry_out = eh->e_entry;
    return 0;
}
