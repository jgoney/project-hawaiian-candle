/* rr-boot-run: run the translated game from reset on the port's own hardware
 * model, with its own timing and no controller input.
 *
 * Usage: rr-boot-run --rom ROM [--snap N:FILE]... [--ram N:FILE]... [--max-frames N]
 *
 * --snap N:FILE   write the picture (PPM) as the Nth VBlank handler entry begins
 * --snap-frame N:FILE  write the picture after N whole frames have run, which
 *                 is what a host that calls "run a frame" N times sees
 * --ram N:FILE    write the 64 KiB work RAM at the same kind of point
 * --max-frames N  give up after N frames (default 3000)
 *
 * N counts entries to the level 6 handler since reset, starting at 1. That is
 * how MAME reference pictures are keyed (scripts/run-mame-boot-pictures-rr206.sh).
 *
 * Exit status: 0 every request was fulfilled; 1 a requested point was not
 * reached; 2 usage or input error, or a fatal runtime error (RR2FATAL).
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "rr_genesis.h"
#include "rr_mem.h"
#include "rr_run.h"

#define RR206_CRC32 0x0876E992u
#define MAX_REQUESTS 64

typedef struct {
    uint64_t vblank;
    const char *path;
    int is_ram;
    int by_frame; /* N counts whole frames run, not VBlank handler entries */
    int done;
} Request;

typedef struct {
    Request requests[MAX_REQUESTS];
    int count, remaining;
} Requests;

static _Noreturn void input_error(const char *fmt, ...) {
    va_list args;
    fprintf(stderr, "rr-boot-run: ");
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fputc('\n', stderr);
    exit(2);
}

static void add_request(Requests *r, const char *text, int is_ram, int by_frame) {
    char *colon;
    unsigned long long n = strtoull(text, &colon, 10);
    if (colon == text || *colon != ':' || colon[1] == 0 || n == 0) {
        input_error("expected N:FILE with N >= 1, got '%s'", text);
    }
    if (r->count == MAX_REQUESTS) input_error("too many --snap/--ram requests");
    r->requests[r->count++] = (Request){n, colon + 1, is_ram, by_frame, 0};
    r->remaining++;
}

/* Called as each VBlank handler is entered, before its first instruction. */
static void on_vblank(RrGenesis *g, void *ctx) {
    Requests *r = ctx;
    for (int i = 0; i < r->count; i++) {
        Request *q = &r->requests[i];
        if (q->done || q->by_frame || q->vblank != g->vblank_entries) continue;
        if (q->is_ram) {
            FILE *f = fopen(q->path, "wb");
            if (f == NULL || fwrite(g->m->ram, 1, sizeof g->m->ram, f) != sizeof g->m->ram) {
                input_error("cannot write %s", q->path);
            }
            fclose(f);
        } else if (rr_genesis_write_ppm(g, q->path) != 0) {
            input_error("cannot write %s", q->path);
        }
        q->done = 1;
        r->remaining--;
    }
}

int main(int argc, char **argv) {
    const char *rom_path = NULL;
    unsigned long long max_frames = 3000;
    static Requests requests;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--rom") == 0 && i + 1 < argc) rom_path = argv[++i];
        else if (strcmp(argv[i], "--snap") == 0 && i + 1 < argc) add_request(&requests, argv[++i], 0, 0);
        else if (strcmp(argv[i], "--snap-frame") == 0 && i + 1 < argc) add_request(&requests, argv[++i], 0, 1);
        else if (strcmp(argv[i], "--ram") == 0 && i + 1 < argc) add_request(&requests, argv[++i], 1, 0);
        else if (strcmp(argv[i], "--max-frames") == 0 && i + 1 < argc) {
            max_frames = strtoull(argv[++i], NULL, 10);
        } else input_error("unknown or incomplete option %s", argv[i]);
    }
    if (rom_path == NULL || requests.count == 0) {
        input_error("usage: rr-boot-run --rom ROM [--snap N:FILE]... [--snap-frame N:FILE]... "
                    "[--ram N:FILE]... [--max-frames N]");
    }

    FILE *f = fopen(rom_path, "rb");
    if (f == NULL) input_error("cannot open %s", rom_path);
    fseek(f, 0, SEEK_END);
    long rom_size = ftell(f);
    rewind(f);
    uint8_t *rom = malloc(rom_size > 0 ? (size_t)rom_size : 1);
    if (rom == NULL || fread(rom, 1, (size_t)rom_size, f) != (size_t)rom_size) {
        input_error("cannot read %s", rom_path);
    }
    fclose(f);
    uint32_t crc = rr_crc32(rom, (size_t)rom_size);
    if (crc != rr_code_rom_crc32) {
        input_error("ROM CRC-32 %08X does not match the translated code (%08X)", crc,
                    rr_code_rom_crc32);
    }
#ifndef RR_ANY_ROM
    if (crc != RR206_CRC32) input_error("ROM CRC-32 %08X is not RR206 (%08X)", crc, RR206_CRC32);
#endif

    RrMachine *m = calloc(1, sizeof *m);
    if (m == NULL) input_error("out of memory");
    m->rom = rom;
    m->rom_size = (uint32_t)rom_size;
    m->rom_crc32 = crc;
    RrGenesis *g = rr_genesis_new(m);
    m->hw = &g->hw;
    g->vblank_hook = on_vblank;
    g->vblank_hook_ctx = &requests;

    rr_genesis_power_on(g);

    clock_t started = clock();
    unsigned long long frames = 0;
    while (requests.remaining > 0 && frames < max_frames) {
        rr_genesis_run_frame(g);
        frames++;
        for (int i = 0; i < requests.count; i++) {
            Request *q = &requests.requests[i];
            if (q->done || !q->by_frame || q->vblank != frames) continue;
            if (rr_genesis_write_ppm(g, q->path) != 0) input_error("cannot write %s", q->path);
            q->done = 1;
            requests.remaining--;
        }
    }
    double seconds = (double)(clock() - started) / CLOCKS_PER_SEC;

    if (requests.remaining > 0) {
        for (int i = 0; i < requests.count; i++) {
            if (!requests.requests[i].done) {
                fprintf(stderr, "rr-boot-run: VBlank entry %llu was not reached in %llu frames "
                                "(%llu entries seen)\n",
                        (unsigned long long)requests.requests[i].vblank, frames,
                        (unsigned long long)g->vblank_entries);
            }
        }
        return 1;
    }
    printf("DONE vblanks=%llu frames=%llu insns=%llu seconds=%.2f\n",
           (unsigned long long)g->vblank_entries, frames, (unsigned long long)m->cpu.insns,
           seconds);
    return 0;
}
