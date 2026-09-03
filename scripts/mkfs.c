/* mkfs — unealta de build (ruleaza pe host, in WSL): impacheteaza fisiere
 * intr-o imagine MyFS.
 *
 * Utilizare: mkfs iesire.img nume=cale [nume=cale ...]
 *
 * Format (vezi kernel/fs.h): "MYFS", u32 numar, intrari de 32 bytes
 * {name[24], u32 lba relativ, u32 size}, apoi datele, totul aliniat la 512. */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct entry {
    char name[24];
    uint32_t lba;
    uint32_t size;
};

static void pad_to_sector(FILE *out)
{
    long pos = ftell(out);
    while (pos % 512) {
        fputc(0, out);
        pos++;
    }
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "utilizare: mkfs iesire.img nume=cale...\n");
        return 1;
    }

    int n = argc - 2;
    if (n > 63) {
        fprintf(stderr, "mkfs: prea multe fisiere (max 63)\n");
        return 1;
    }
    struct entry *e = calloc((size_t)n, sizeof *e);
    FILE **in = calloc((size_t)n, sizeof *in);

    /* Tabelul are o regiune FIXA de 4 sectoare, ca kernelul sa poata
     * adauga fisiere ulterior fara sa mute datele (vezi kernel/fs.c). */
    uint32_t table_sectors = 4;
    uint32_t lba = table_sectors;

    for (int i = 0; i < n; i++) {
        char *spec = argv[i + 2];
        char *eq = strchr(spec, '=');
        if (!eq) {
            fprintf(stderr, "mkfs: argument invalid '%s' (astept nume=cale)\n", spec);
            return 1;
        }
        *eq = '\0';
        if (strlen(spec) > 23) {
            fprintf(stderr, "mkfs: nume prea lung '%s' (max 23)\n", spec);
            return 1;
        }
        strncpy(e[i].name, spec, 23);

        in[i] = fopen(eq + 1, "rb");
        if (!in[i]) {
            fprintf(stderr, "mkfs: nu pot deschide '%s'\n", eq + 1);
            return 1;
        }
        fseek(in[i], 0, SEEK_END);
        long sz = ftell(in[i]);
        fseek(in[i], 0, SEEK_SET);

        e[i].lba  = lba;
        e[i].size = (uint32_t)sz;
        lba += (uint32_t)((sz + 511) / 512);
    }

    FILE *out = fopen(argv[1], "wb");
    if (!out) {
        fprintf(stderr, "mkfs: nu pot scrie '%s'\n", argv[1]);
        return 1;
    }

    fwrite("MYFS", 1, 4, out);
    uint32_t cnt = (uint32_t)n;
    fwrite(&cnt, 4, 1, out);
    fwrite(e, sizeof *e, (size_t)n, out);
    /* completam pana la capatul regiunii fixe a tabelului */
    while (ftell(out) < (long)(table_sectors * 512))
        fputc(0, out);

    for (int i = 0; i < n; i++) {
        int c;
        while ((c = fgetc(in[i])) != EOF)
            fputc(c, out);
        pad_to_sector(out);
        fclose(in[i]);
    }

    fclose(out);
    printf("mkfs: %d fisiere, %u sectoare\n", n, lba);
    return 0;
}
