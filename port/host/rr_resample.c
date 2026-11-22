#include "rr_resample.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define REACH 24         /* input frames the filter looks at on each side */
#define CUTOFF 0.875     /* of half the lower rate: flat to about 18 kHz at 48,000 */
#define PI 3.14159265358979323846

struct RrResample {
    double in_hz;
    float *held;      /* input frames not yet finished with, interleaved */
    size_t capacity;  /* frames `held` has room for */
    size_t count;     /* frames in it */
    double position;  /* where the next output frame falls, in input frames from held[0] */
};

RrResample *rr_resample_new(double in_hz) {
    RrResample *r = calloc(1, sizeof *r);
    if (r == NULL) return NULL;
    r->in_hz = in_hz;
    r->capacity = (size_t)in_hz + 4 * REACH; /* about a second */
    r->held = malloc(r->capacity * 2 * sizeof *r->held);
    if (r->held == NULL) {
        free(r);
        return NULL;
    }
    rr_resample_reset(r);
    return r;
}

void rr_resample_free(RrResample *r) {
    if (r == NULL) return;
    free(r->held);
    free(r);
}

void rr_resample_reset(RrResample *r) {
    /* Silence before the first frame, so the filter has something to look back on. */
    memset(r->held, 0, (size_t)REACH * 2 * sizeof *r->held);
    r->count = REACH;
    r->position = REACH;
}

/* One output frame at `position`: the input around it, weighted by a
 * windowed sinc (Blackman window) that passes up to `cutoff` cycles per
 * input frame. The weights are scaled to add up to 1. */
static void one_frame(const RrResample *r, double cutoff, float *out) {
    long centre = (long)floor(r->position);
    double left = 0, right = 0, total = 0;
    for (long i = centre - REACH + 1; i <= centre + REACH; i++) {
        double x = (double)i - r->position;
        double angle = 2 * PI * cutoff * x;
        double sinc = fabs(angle) < 1e-9 ? 1.0 : sin(angle) / angle;
        double w = x / REACH; /* -1..1 across the window */
        double window = 0.42 + 0.5 * cos(PI * w) + 0.08 * cos(2 * PI * w);
        double weight = sinc * window;
        left += weight * r->held[2 * i];
        right += weight * r->held[2 * i + 1];
        total += weight;
    }
    out[0] = (float)(left / total);
    out[1] = (float)(right / total);
}

size_t rr_resample_run(RrResample *r, double out_hz, const int16_t *in, size_t in_frames,
                       size_t *used, float *out, size_t max_out) {
    *used = 0;
    if (!(out_hz >= RR_RESAMPLE_MIN_HZ && out_hz <= RR_RESAMPLE_MAX_HZ)) return 0;
    size_t room = r->capacity - r->count;
    size_t take = in_frames < room ? in_frames : room;
    for (size_t i = 0; i < 2 * take; i++) r->held[2 * r->count + i] = (float)in[i] / 32768.0f;
    r->count += take;
    *used = take;

    double step = r->in_hz / out_hz;
    double cutoff = 0.5 * CUTOFF * (step > 1.0 ? 1.0 / step : 1.0);
    size_t made = 0;
    /* A frame can be made once the input reaches REACH past it. */
    while (made < max_out && floor(r->position) + REACH < (double)r->count) {
        one_frame(r, cutoff, out + 2 * made);
        made++;
        r->position += step;
    }
    /* Let go of input the filter can no longer reach. */
    long keep_from = (long)floor(r->position) - REACH + 1;
    if (keep_from > 0) {
        size_t drop = (size_t)keep_from < r->count ? (size_t)keep_from : r->count;
        memmove(r->held, r->held + 2 * drop, (r->count - drop) * 2 * sizeof *r->held);
        r->count -= drop;
        r->position -= (double)drop;
    }
    return made;
}
