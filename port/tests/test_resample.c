/* Unit tests for the sample-rate converter. Run with: make -C port test */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rr_resample.h"

#define IN_HZ 53267.0
#define PI 3.14159265358979323846

static int16_t in[2 * 600000];
static float out[2 * 600000], other[2 * 600000];

static void sine(double hz, size_t frames, double left, double right) {
    for (size_t i = 0; i < frames; i++) {
        double s = sin(2 * PI * hz * (double)i / IN_HZ);
        in[2 * i] = (int16_t)lrint(left * s);
        in[2 * i + 1] = (int16_t)lrint(right * s);
    }
}

static size_t convert(double out_hz, size_t frames, size_t chunk, float *to) {
    RrResample *r = rr_resample_new(IN_HZ);
    size_t made = 0;
    for (size_t at = 0; at < frames;) {
        size_t n = frames - at < chunk ? frames - at : chunk, used = 0;
        made += rr_resample_run(r, out_hz, in + 2 * at, n, &used, to + 2 * made, 600000 - made);
        assert(used == n);
        at += n;
    }
    rr_resample_free(r);
    return made;
}

static double peak(const float *samples, size_t from, size_t to, int side) {
    double most = 0;
    for (size_t i = from; i < to; i++) {
        if (fabs(samples[2 * i + side]) > most) most = fabs(samples[2 * i + side]);
    }
    return most;
}

static void test_a_steady_level_stays(void) {
    for (size_t i = 0; i < 20000; i++) {
        in[2 * i] = 16384;
        in[2 * i + 1] = -8192;
    }
    size_t made = convert(48000, 20000, 20000, out);
    assert(made > 17000);
    for (size_t i = 200; i < made; i++) {
        assert(fabs(out[2 * i] - 0.5) < 0.005);
        assert(fabs(out[2 * i + 1] + 0.25) < 0.0025); /* the two sides stay apart */
    }
}

static void test_a_tone_keeps_its_level_and_pitch(void) {
    sine(1000, 53267, 16384, 0);
    size_t made = convert(48000, 53267, 889, out);
    assert(fabs(peak(out, 1000, made, 0) - 0.5) < 0.015);
    assert(peak(out, 1000, made, 1) < 0.001);
    int crossings = 0;
    for (size_t i = 1; i < made; i++) crossings += (out[2 * (i - 1)] < 0) != (out[2 * i] < 0);
    assert(crossings >= 1996 && crossings <= 2002); /* 1,000 Hz for a second */
    /* The same at another card rate. */
    made = convert(44100, 53267, 889, out);
    assert(fabs(peak(out, 1000, made, 0) - 0.5) < 0.015);
    crossings = 0;
    for (size_t i = 1; i < made; i++) crossings += (out[2 * (i - 1)] < 0) != (out[2 * i] < 0);
    assert(crossings >= 1996 && crossings <= 2002);
}

/* A tone above half the output rate would come out as a false lower tone. */
static void test_what_the_card_cannot_carry_is_removed(void) {
    sine(25000, 53267, 16384, 16384);
    size_t made = convert(48000, 53267, 889, out);
    assert(peak(out, 1000, made, 0) < 0.5 / 31.6); /* 30 dB down */
    /* A high tone the card can carry is kept. */
    sine(15000, 53267, 16384, 16384);
    made = convert(48000, 53267, 889, out);
    assert(peak(out, 1000, made, 0) > 0.45);
}

static void test_the_amount_follows_the_rates(void) {
    memset(in, 0, sizeof in);
    size_t made = convert(48000, 532670, 889, out);
    double want = 532670.0 * 48000 / IN_HZ;
    assert(fabs((double)made - want) < 60); /* less the filter's own reach */
    size_t at_44 = convert(44100, 532670, 889, out);
    assert(fabs((double)at_44 - 532670.0 * 44100 / IN_HZ) < 60);
}

static void test_chunks_do_not_matter(void) {
    sine(3000, 20000, 12000, 7000);
    size_t whole = convert(48000, 20000, 20000, out);
    static const size_t chunks[] = {1, 7, 889};
    for (int c = 0; c < 3; c++) {
        size_t made = convert(48000, 20000, chunks[c], other);
        assert(made == whole);
        for (size_t i = 0; i < 2 * made; i++) assert(fabs(out[i] - other[i]) < 1e-6);
    }
}

static void test_bad_rates_do_nothing(void) {
    RrResample *r = rr_resample_new(IN_HZ);
    size_t used = 99;
    static const double bad[] = {0, -48000, 100, 1e9, NAN};
    for (int i = 0; i < 5; i++) {
        assert(rr_resample_run(r, bad[i], in, 100, &used, out, 100) == 0 && used == 0);
    }
    rr_resample_free(r);
}

static void test_output_room_is_respected_and_reset_forgets(void) {
    RrResample *r = rr_resample_new(IN_HZ);
    size_t used = 0;
    for (size_t i = 0; i < 4000; i++) in[2 * i] = in[2 * i + 1] = 20000;
    assert(rr_resample_run(r, 48000, in, 4000, &used, out, 100) == 100 && used == 4000);
    size_t rest = rr_resample_run(r, 48000, in, 0, &used, out, 600000);
    assert(rest > 3000 && used == 0); /* what did not fit waited */
    rr_resample_reset(r);
    memset(in, 0, 2 * 200 * sizeof *in);
    size_t made = rr_resample_run(r, 48000, in, 200, &used, out, 600000);
    for (size_t i = 0; i < 2 * made; i++) assert(out[i] == 0.0f); /* none of the old level */
    rr_resample_free(r);
}

int main(void) {
    test_a_steady_level_stays();
    test_a_tone_keeps_its_level_and_pitch();
    test_what_the_card_cannot_carry_is_removed();
    test_the_amount_follows_the_rates();
    test_chunks_do_not_matter();
    test_bad_rates_do_nothing();
    test_output_room_is_respected_and_reset_forgets();
    printf("test_resample: all passed\n");
    return 0;
}
