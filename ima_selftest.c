/* ima_selftest.c -- headless regression harness for ima_core.
 *
 * Prints a CRC32 (zlib-compatible) of the CA buffer after each generation,
 * driving the engine exactly as the zoomer does with the focus centered:
 *
 *     ./ima_selftest [seed [generations [width height]]]
 *
 * `make selftest` compares the output for two CA sizes against
 * selftest.expected, recorded from this engine.
 */
#include <stdio.h>
#include <stdlib.h>
#include "ima_core.h"

static unsigned int crc_table[256];

static void crc_init(void)
{
    unsigned int i, j, c;
    for (i = 0; i < 256; i++) {
        c = i;
        for (j = 0; j < 8; j++)
            c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[i] = c;
    }
}

static unsigned int crc32_buf(const unsigned char *p, size_t n)
{
    unsigned int c = 0xFFFFFFFFu;
    while (n--)
        c = crc_table[(c ^ *p++) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

/* CRC of the image as little-endian 16-bit words (matches numpy '<u2'). */
static unsigned int image_crc(const Ima *m)
{
    size_t n = (size_t)m->width * m->height, i;
    unsigned char *tmp = malloc(n * 2);
    unsigned int c;
    for (i = 0; i < n; i++) {
        tmp[2 * i]     = (unsigned char)(m->front[i] & 0xFF);
        tmp[2 * i + 1] = (unsigned char)(m->front[i] >> 8);
    }
    c = crc32_buf(tmp, n * 2);
    free(tmp);
    return c;
}

int main(int argc, char **argv)
{
    unsigned int seed = (argc > 1) ? (unsigned int)strtoul(argv[1], 0, 10) : 12345;
    int frames = (argc > 2) ? atoi(argv[2]) : 12;
    int width  = (argc > 3) ? atoi(argv[3]) : 768;
    int height = (argc > 4) ? atoi(argv[4]) : 512;
    unsigned int cycles;   /* zoomer this[1]: completed generations */
    Ima m;

    crc_init();
    if (!ima_create(&m, width, height)) {
        fprintf(stderr, "out of memory\n");
        return 1;
    }
    ima_seed(&m, seed);
    /* focus at center = mouse centered in the original */
    ima_set_focus(&m, width / 4, height / 4);

    printf("seed %u size %dx%d\n", seed, width, height);
    printf("gen 0 crc %08x\n", image_crc(&m));
    for (cycles = 0; cycles < (unsigned int)frames; cycles++) {
        /* generation, exactly as the zoomer drives it (sub_406690/4065C0) */
        if (cycles > 9)
            ima_update_row_offsets(&m);
        ima_swap(&m);
        ima_evolve_rows(&m, 0, height / 2 - 1);
        printf("gen %u crc %08x\n", cycles + 1, image_crc(&m));
    }
    ima_destroy(&m);
    return 0;
}
