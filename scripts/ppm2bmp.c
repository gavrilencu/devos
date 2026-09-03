/* ppm2bmp — unealta de build/test: converteste un screendump PPM (P6)
 * de la QEMU intr-un BMP 24bpp, ca sa-l putem transforma usor in PNG. */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

static void wr32(FILE *f, uint32_t v) { fwrite(&v, 4, 1, f); }
static void wr16(FILE *f, uint16_t v) { fwrite(&v, 2, 1, f); }

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "utilizare: ppm2bmp in.ppm out.bmp\n");
        return 1;
    }
    FILE *in = fopen(argv[1], "rb");
    if (!in) { fprintf(stderr, "nu pot deschide %s\n", argv[1]); return 1; }

    int w, h, maxv;
    if (fscanf(in, "P6 %d %d %d", &w, &h, &maxv) != 3) {
        fprintf(stderr, "nu e PPM P6\n");
        return 1;
    }
    fgetc(in);   /* newline-ul de dupa header */

    uint8_t *px = malloc((size_t)w * h * 3);
    if (fread(px, 3, (size_t)w * h, in) != (size_t)w * h) {
        fprintf(stderr, "date incomplete\n");
        return 1;
    }
    fclose(in);

    FILE *out = fopen(argv[2], "wb");
    int stride = (w * 3 + 3) & ~3;
    uint32_t datasz = (uint32_t)stride * h;

    fwrite("BM", 1, 2, out);
    wr32(out, 54 + datasz);
    wr32(out, 0);
    wr32(out, 54);
    wr32(out, 40);
    wr32(out, (uint32_t)w);
    wr32(out, (uint32_t)h);
    wr16(out, 1);
    wr16(out, 24);
    wr32(out, 0);
    wr32(out, datasz);
    wr32(out, 2835); wr32(out, 2835);
    wr32(out, 0); wr32(out, 0);

    uint8_t *line = calloc(1, (size_t)stride);
    for (int y = h - 1; y >= 0; y--) {           /* BMP e de jos in sus */
        for (int x = 0; x < w; x++) {
            const uint8_t *s = px + ((size_t)y * w + x) * 3;
            line[x * 3 + 0] = s[2];              /* BGR */
            line[x * 3 + 1] = s[1];
            line[x * 3 + 2] = s[0];
        }
        fwrite(line, 1, (size_t)stride, out);
    }
    fclose(out);
    return 0;
}
