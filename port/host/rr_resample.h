/* Turns the sound chip's samples (16-bit stereo at its own rate) into the
 * sound card's (32-bit float stereo, -1 to 1, at whatever rate is asked),
 * with a low-pass filter so that nothing above half the output rate folds
 * back as a false tone. The asked rate may change a little from call to
 * call (the window does that to stay in step); the position carries on. */
#ifndef RR_RESAMPLE_H
#define RR_RESAMPLE_H

#include <stddef.h>
#include <stdint.h>

#define RR_RESAMPLE_MIN_HZ 8000.0
#define RR_RESAMPLE_MAX_HZ 384000.0

typedef struct RrResample RrResample;

RrResample *rr_resample_new(double in_hz);
void rr_resample_free(RrResample *r);
void rr_resample_reset(RrResample *r); /* forget everything fed so far */
/* Feed `in_frames` stereo frames and write at most `max_out` frames at
 * `out_hz`. Returns the frames written; *used is how many input frames were
 * taken (all of them unless about a second is already waiting). A rate
 * outside RR_RESAMPLE_MIN_HZ..MAX_HZ writes and takes nothing. */
size_t rr_resample_run(RrResample *r, double out_hz, const int16_t *in, size_t in_frames,
                       size_t *used, float *out, size_t max_out);

#endif
