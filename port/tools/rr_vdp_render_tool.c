/* rr-vdp-render: render a dumped video chip state with the port's renderer.
 *
 * Usage: rr-vdp-render --state FILE -o PICTURE.ppm
 *
 * State file (written by mame/rr206-vdp-state.lua from MAME's own video
 * memory): "RR2VDPSTATE1\n", 24 register bytes, 65,536 bytes of VRAM, 64
 * big-endian CRAM words, 40 big-endian VSRAM words.
 *
 * Exit status: 0 written; 2 usage or input error.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rr_run.h"
#include "rr_vdp.h"

#ifdef RR_NO_GENERATED_CODE
/* Built on its own, without translated game code. */
const uint32_t rr_code_rom_crc32 = 0;
RrInsnFn rr_lookup(uint32_t pc) { (void)pc; return NULL; }
#endif

static const char MAGIC[] = "RR2VDPSTATE1\n";

static int read_part(FILE *f, void *out, size_t size, const char *what, const char *path) {
    if (fread(out, 1, size, f) != size) {
        fprintf(stderr, "rr-vdp-render: %s is truncated: %s is incomplete\n", path, what);
        return 0;
    }
    return 1;
}

int main(int argc, char **argv) {
    const char *state_path = NULL, *out_path = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--state") == 0 && i + 1 < argc) state_path = argv[++i];
        else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) out_path = argv[++i];
        else state_path = out_path = NULL, i = argc;
    }
    if (state_path == NULL || out_path == NULL) {
        fprintf(stderr, "usage: rr-vdp-render --state FILE -o PICTURE.ppm\n");
        return 2;
    }
    FILE *f = fopen(state_path, "rb");
    if (f == NULL) {
        fprintf(stderr, "rr-vdp-render: cannot open %s\n", state_path);
        return 2;
    }
    static RrVdp vdp;
    rr_vdp_reset(&vdp);
    char magic[sizeof MAGIC - 1];
    uint8_t words[64 * 2];
    if (fread(magic, 1, sizeof magic, f) != sizeof magic || memcmp(magic, MAGIC, sizeof magic) != 0) {
        fprintf(stderr, "rr-vdp-render: %s is not an RR2VDPSTATE1 file\n", state_path);
        return 2;
    }
    if (!read_part(f, vdp.reg, sizeof vdp.reg, "the register block", state_path)) return 2;
    if (!read_part(f, vdp.vram, sizeof vdp.vram, "VRAM", state_path)) return 2;
    if (!read_part(f, words, 64 * 2, "CRAM", state_path)) return 2;
    for (int i = 0; i < 64; i++) vdp.cram[i] = (uint16_t)(words[i * 2] << 8 | words[i * 2 + 1]);
    if (!read_part(f, words, RR_VDP_VSRAM_ENTRIES * 2, "VSRAM", state_path)) return 2;
    for (int i = 0; i < RR_VDP_VSRAM_ENTRIES; i++) {
        vdp.vsram[i] = (uint16_t)(words[i * 2] << 8 | words[i * 2 + 1]);
    }
    fclose(f);
    if (rr_vdp_write_ppm(&vdp, out_path) != 0) {
        fprintf(stderr, "rr-vdp-render: cannot write %s\n", out_path);
        return 2;
    }
    return 0;
}
