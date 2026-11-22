/* rr-objects-draw ROM RAM OUT
 *
 * Runs the C repeat of the game's object drawing (port/render/rr_objects.h)
 * on a work-RAM image (0x10000 bytes, taken where the game is about to draw)
 * and writes RAM as the drawing leaves it to OUT, so it can be compared with
 * the Python reference or with the game. Prints what it found:
 *
 *   ok N
 *   sprites FIRST END
 *   tiles FIRST END
 *   picture I OFFSET WIDTH HEIGHT STRIDE SIZE X Y MIRRORED FIRST_COLUMN FIRST_ROW COLUMNS ROWS
 *   entry I PICTURE DX
 *
 * OFFSET is where the picture's rows start in the ROM. Exit 0, or 2 with a
 * message when a file cannot be read or written.
 */
#include <stdio.h>
#include <stdlib.h>

#include "rr_objects.h"

static uint8_t *read_file(const char *path, long *size) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) return NULL;
    fseek(f, 0, SEEK_END);
    *size = ftell(f);
    rewind(f);
    uint8_t *data = *size > 0 ? malloc((size_t)*size) : NULL;
    if (data != NULL && fread(data, 1, (size_t)*size, f) != (size_t)*size) {
        free(data);
        data = NULL;
    }
    fclose(f);
    return data;
}

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: rr-objects-draw ROM RAM OUT\n");
        return 2;
    }
    long rom_size = 0, ram_size = 0;
    uint8_t *rom = read_file(argv[1], &rom_size);
    if (rom == NULL) {
        fprintf(stderr, "rr-objects-draw: cannot read %s\n", argv[1]);
        return 2;
    }
    uint8_t *ram = read_file(argv[2], &ram_size);
    if (ram == NULL || ram_size != 0x10000) {
        fprintf(stderr, "rr-objects-draw: %s is not a 65,536-byte work-RAM image\n", argv[2]);
        return 2;
    }
    static RrObjectSet set;
    static uint8_t work[0x10000];
    rr_objects_draw(ram, rom, (size_t)rom_size, &set, work);
    FILE *out = fopen(argv[3], "wb");
    if (out == NULL || fwrite(work, 1, sizeof work, out) != sizeof work || fclose(out) != 0) {
        fprintf(stderr, "rr-objects-draw: cannot write %s\n", argv[3]);
        return 2;
    }
    printf("ok %d\n", set.ok);
    if (!set.ok) return 0;
    printf("sprites %d %d\n", set.first_sprite, set.sprites);
    printf("tiles %d %d\n", set.first_tile, set.tiles);
    for (int i = 0; i < set.pictures; i++) {
        const RrFinePicture *p = &set.picture[i];
        printf("picture %d %ld %u %u %u %u %d %d %u %u %u %u %u\n", i, (long)(p->rows - rom), p->width,
               p->height, p->stride, (unsigned)p->size, p->x, p->y, p->mirrored, p->first_column,
               p->first_row, p->columns, p->rows_shown);
    }
    for (int i = set.first_sprite; i < set.sprites; i++)
        printf("entry %d %d %d\n", i, set.fine.picture[i], set.fine.dx[i]);
    return 0;
}
