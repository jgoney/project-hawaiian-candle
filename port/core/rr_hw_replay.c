/* Hardware backed by a recorded access log.
 *
 * Log format (hw.txt), one access per line in the order the 68000 made them:
 *   R|W <addr hex> <size 1|2> <value hex>
 */
#include <stdlib.h>
#include <string.h>

#include "rr_run.h"

typedef struct {
    uint32_t addr;
    uint16_t value;
    uint8_t size;
    char kind;
} Access;

typedef struct {
    RrHw hw; /* must be first */
    const RrMachine *m;
    Access *log;
    size_t count, pos;
} Replay;

static const Access *next(Replay *r, char kind, uint32_t addr, int size) {
    if (r->pos >= r->count) {
        rr_fatal(r->m, "hardware log exhausted at %c %06X size %d (record %zu)", kind, addr, size,
                 r->pos);
    }
    const Access *a = &r->log[r->pos];
    if (a->kind != kind || a->addr != addr || a->size != size) {
        rr_fatal(r->m, "hardware replay out of step at record %zu: code did %c %06X size %d, "
                       "recording has %c %06X size %d",
                 r->pos, kind, addr, size, a->kind, a->addr, a->size);
    }
    r->pos++;
    return a;
}

static uint16_t replay_read(RrHw *hw, uint32_t addr, int size) {
    return next((Replay *)hw, 'R', addr, size)->value;
}

static void replay_write(RrHw *hw, uint32_t addr, int size, uint16_t value) {
    Replay *r = (Replay *)hw;
    const Access *a = next(r, 'W', addr, size);
    if (a->value != value) {
        rr_fatal(r->m, "hardware write differs at record %zu: code wrote %04X to %06X, "
                       "recording has %04X",
                 r->pos - 1, value, addr, a->value);
    }
}

static void replay_close(RrHw *hw) {
    Replay *r = (Replay *)hw;
    free(r->log);
    free(r);
}

RrHw *rr_hw_replay_open(const char *path, const RrMachine *m) {
    FILE *f = fopen(path, "r");
    if (f == NULL) rr_fatal(m, "cannot open hardware log %s", path);
    Replay *r = calloc(1, sizeof *r);
    size_t cap = 0;
    char line[128];
    size_t line_number = 0;
    if (r == NULL) rr_fatal(m, "out of memory");
    while (fgets(line, sizeof line, f) != NULL) {
        char kind;
        unsigned addr, size, value;
        line_number++;
        if (sscanf(line, " %c %x %u %x", &kind, &addr, &size, &value) != 4 ||
            (kind != 'R' && kind != 'W') || (size != 1 && size != 2)) {
            rr_fatal(m, "hardware log %s line %zu is malformed", path, line_number);
        }
        if (r->count == cap) {
            cap = cap ? cap * 2 : 4096;
            r->log = realloc(r->log, cap * sizeof *r->log);
            if (r->log == NULL) rr_fatal(m, "out of memory");
        }
        r->log[r->count++] = (Access){addr, (uint16_t)value, (uint8_t)size, kind};
    }
    fclose(f);
    r->m = m;
    r->hw.read = replay_read;
    r->hw.write = replay_write;
    r->hw.close = replay_close;
    return &r->hw;
}

size_t rr_hw_replay_remaining(RrHw *hw) {
    Replay *r = (Replay *)hw;
    return r->count - r->pos;
}

size_t rr_hw_replay_position(RrHw *hw) { return ((Replay *)hw)->pos; }

void rr_hw_replay_seek(RrHw *hw, size_t position) {
    Replay *r = (Replay *)hw;
    if (position > r->count) rr_fatal(r->m, "hardware log seek past end");
    r->pos = position;
}
