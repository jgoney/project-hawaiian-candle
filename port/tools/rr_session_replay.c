/* Do a session saved by the Godot window again (rr_console.h: "the session"),
 * to bring the console to exactly the moment that was captured.
 *
 * rr-session-replay --rom ROM --session FILE [--ram FILE] [--picture FILE]
 *                   [--last N:PREFIX] [--plain]
 *                   [--at FRAME:FILE]...
 *
 * Prints "SESSION frames=N". With --ram FILE (the 65,536 bytes of work RAM
 * saved with the session) it compares its own work RAM at the end with it and
 * prints "RAM identical" or "RAM differing_bytes=K first=0xFFxxxx".
 * --picture FILE writes the picture at the end, at the session's scale, as a
 * PPM; --last N:PREFIX writes the last N frames' pictures as
 * PREFIX-<frame>.ppm, and --at FRAME:FILE (up to 64 times) the picture after
 * that many frames. With --plain, each picture written is followed by the
 * game's own 320 x 224 picture of the same frame, as the same name with
 * ".plain.ppm" added: what the video chip shows before the crisp road, the
 * full-size objects and the in-between pictures. Ends with "RESULT reproduced" (the RAM is identical),
 * "RESULT different" (exit 1) or, without --ram, "RESULT replayed". Bad
 * input and a session that cannot be replayed are exit 2.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rr_console.h"

static void input_error(const char *message, const char *detail) {
    fprintf(stderr, "rr-session-replay: %s%s%s\n", message, detail != NULL ? ": " : "", detail != NULL ? detail : "");
    exit(2);
}

static char *read_file(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) input_error("cannot open", path);
    fseek(f, 0, SEEK_END);
    long length = ftell(f);
    rewind(f);
    char *data = malloc((size_t)(length > 0 ? length : 0) + 1);
    if (data == NULL || fread(data, 1, (size_t)length, f) != (size_t)length) input_error("cannot read", path);
    fclose(f);
    data[length] = 0;
    *size = (size_t)length;
    return data;
}

static int also_plain; /* --plain */

static void write_pixels(const uint32_t *pixels, int w, int h, const char *path);

static void write_picture(const RrConsole *c, const char *path) {
    int scale = rr_console_scale(c);
    write_pixels(rr_console_scaled_frame(c), rr_console_scaled_width(c), RR_CONSOLE_H * scale, path);
    if (also_plain) {
        char plain[1100];
        snprintf(plain, sizeof plain, "%s.plain.ppm", path);
        write_pixels(rr_console_frame(c), RR_CONSOLE_W, RR_CONSOLE_H, plain);
    }
}

static void write_pixels(const uint32_t *pixels, int w, int h, const char *path) {
    FILE *f = fopen(path, "wb");
    if (f == NULL) input_error("cannot write", path);
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int i = 0; i < w * h; i++) {
        unsigned char rgb[3] = {(unsigned char)(pixels[i] >> 16), (unsigned char)(pixels[i] >> 8), (unsigned char)pixels[i]};
        fwrite(rgb, 1, 3, f);
    }
    if (fclose(f) != 0) input_error("cannot write", path);
}

enum { AT_MAX = 64 };
typedef struct {
    unsigned long long end, last;
    const char *prefix;
    unsigned long long at_frame[AT_MAX];
    const char *at_path[AT_MAX];
    int at_count;
} Last;

static void each_frame(void *ctx, RrConsole *c) {
    const Last *last = ctx;
    unsigned long long frame = rr_console_session_frames(c);
    for (int i = 0; i < last->at_count; i++) {
        if (last->at_frame[i] == frame) write_picture(c, last->at_path[i]);
    }
    if (last->prefix == NULL || frame + last->last <= last->end) return;
    char path[1024];
    snprintf(path, sizeof path, "%s-%llu.ppm", last->prefix, frame);
    write_picture(c, path);
}

int main(int argc, char **argv) {
    const char *rom = NULL, *session = NULL, *ram = NULL, *picture = NULL;
    static Last last;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--rom") == 0 && i + 1 < argc) rom = argv[++i];
        else if (strcmp(argv[i], "--session") == 0 && i + 1 < argc) session = argv[++i];
        else if (strcmp(argv[i], "--ram") == 0 && i + 1 < argc) ram = argv[++i];
        else if (strcmp(argv[i], "--picture") == 0 && i + 1 < argc) picture = argv[++i];
        else if (strcmp(argv[i], "--plain") == 0) also_plain = 1;
        else if (strcmp(argv[i], "--at") == 0 && i + 1 < argc) {
            char *end;
            unsigned long long frame = strtoull(argv[++i], &end, 10);
            if (end == argv[i] || *end != ':' || end[1] == 0 || last.at_count == AT_MAX) input_error("--at needs FRAME:FILE, at most 64 times", argv[i]);
            last.at_frame[last.at_count] = frame;
            last.at_path[last.at_count++] = end + 1;
        }
        else if (strcmp(argv[i], "--last") == 0 && i + 1 < argc) {
            char *end;
            last.last = strtoull(argv[++i], &end, 10);
            if (end == argv[i] || *end != ':' || end[1] == 0 || last.last == 0) input_error("--last needs N:PREFIX", argv[i]);
            last.prefix = end + 1;
        } else input_error("unknown or incomplete option", argv[i]);
    }
    if (rom == NULL || session == NULL) {
        input_error("usage: rr-session-replay --rom ROM --session FILE [--ram FILE] [--picture FILE] [--last N:PREFIX] [--plain] [--at FRAME:FILE]...", NULL);
    }
    size_t text_size, ram_size = 0;
    char *text = read_file(session, &text_size);
    char *saved = ram != NULL ? read_file(ram, &ram_size) : NULL;
    if (saved != NULL && ram_size != 65536) input_error("the RAM file is not 65,536 bytes", ram);
    if (strlen(text) != text_size) input_error("not a session (it is not text)", session);
    /* The end line says how many frames there are, for --last. */
    const char *end_line = strstr(text, "\nend ");
    if (end_line != NULL) last.end = strtoull(end_line + 5, NULL, 10);

    char error[512];
    RrConsole *c = rr_console_open(rom, error, sizeof error);
    if (c == NULL) input_error(error, NULL);
    if (!rr_console_replay_session(c, text, each_frame, &last, error, sizeof error)) input_error(session, error);
    printf("SESSION frames=%llu\n", (unsigned long long)rr_console_session_frames(c));
    int result = 0;
    const char *verdict = "replayed";
    if (saved != NULL) {
        const uint8_t *now = rr_console_ram(c);
        long differing = 0, first = -1;
        for (long i = 0; i < 65536; i++) {
            if (now[i] != (uint8_t)saved[i]) {
                if (first < 0) first = i;
                differing++;
            }
        }
        if (differing == 0) printf("RAM identical\n");
        else printf("RAM differing_bytes=%ld first=0xFF%04lX\n", differing, first);
        verdict = differing == 0 ? "reproduced" : "different";
        result = differing != 0;
    }
    if (picture != NULL) {
        write_picture(c, picture);
        printf("PICTURE %s %dx%d\n", picture, rr_console_scaled_width(c), RR_CONSOLE_H * rr_console_scale(c));
    }
    printf("RESULT %s\n", verdict);
    rr_console_close(c);
    free(text);
    free(saved);
    return result;
}
