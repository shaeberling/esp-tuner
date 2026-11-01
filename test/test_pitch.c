/*
 * Host tests for main/pitch.c on synthetic signals: accuracy in cents,
 * octave errors, silence and noise, two notes, and speed.
 *   make -C test
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "pitch.h"

static float x[PITCH_N];
static int failures;

static unsigned s_rng = 12345;
static float frand(void)   /* -1 .. 1 */
{
    s_rng = s_rng * 1664525u + 1013904223u;
    return (float)((s_rng >> 8) & 0xFFFFFF) / 8388608.0f - 1.0f;
}

static float cents(float a, float b) { return 1200.0f * log2f(a / b); }

static void tone(float f0, const float *amps, int n_harm, float inharm, float decay_s, float noise)
{
    float ph[32];
    for (int h = 0; h < n_harm; h++) {
        ph[h] = (frand() + 1.0f) * (float)M_PI;
    }
    for (int i = 0; i < PITCH_N; i++) {
        const double t = (double)i / PITCH_FS;
        double v = 0;
        for (int h = 0; h < n_harm; h++) {
            const int k = h + 1;
            const double fh = k * f0 * sqrt(1.0 + inharm * k * k);
            v += amps[h] * sin(2 * M_PI * fh * t + ph[h]);
        }
        if (decay_s > 0) {
            v *= exp(-t / decay_s);
        }
        x[i] = (float)v + noise * frand();
    }
}

static void check(const char *what, bool ok)
{
    if (!ok) {
        failures++;
        printf("  FAIL: %s\n", what);
    }
}

int main(void)
{
    if (!pitch_init()) {
        printf("init failed\n");
        return 1;
    }
    pitch_result_t r;
    const float one[] = { 1.0f };

    /* 1. Pure sines over the whole range, off-grid. */
    float worst = 0;
    for (float f = 41.0f; f < 2000.0f; f *= 1.0313f) {
        tone(f, (const float[]){ 0.3f }, 1, 0, 0, 0.001f);
        pitch_analyze(x, &r);
        const float e = r.hz > 0 ? cents(r.hz, f) : 9999;
        worst = fmaxf(worst, fabsf(e));
        if (fabsf(e) > 0.2f) {
            printf("  sine %.2f Hz -> %.3f Hz (%.2f cents, clarity %.3f)\n", f, r.hz, e, r.clarity);
        }
    }
    printf("sines 41-2000 Hz: worst error %.3f cents\n", worst);
    check("sines within 0.2 cent", worst < 0.2f);

    /* 2. Strings: weak fundamental, loud 2nd harmonic, inharmonic, decaying. */
    const float guitar[] = { 0.25f, 1.0f, 0.7f, 0.5f, 0.35f, 0.25f, 0.2f, 0.15f, 0.1f, 0.08f };
    const struct { const char *name; float hz; } strings[] = {
        { "E1", 41.20f }, { "A1", 55.0f }, { "D2", 73.42f }, { "G2", 98.0f },
        { "E2", 82.41f }, { "A2", 110.0f }, { "D3", 146.83f }, { "G3", 196.0f },
        { "B3", 246.94f }, { "E4", 329.63f }, { "A4", 440.0f }, { "E5", 659.26f }, { "C6", 1046.5f },
    };
    worst = 0;
    for (size_t i = 0; i < sizeof(strings) / sizeof(strings[0]); i++) {
        tone(strings[i].hz, guitar, 10, 0.00008f, 0.8f, 0.002f);
        pitch_analyze(x, &r);
        const float e = r.hz > 0 ? cents(r.hz, strings[i].hz) : 9999;
        worst = fmaxf(worst, fabsf(e));
        printf("  string %-3s %8.2f Hz -> %8.3f Hz  %+6.2f cents  clarity %.3f  2nd %.1f Hz (H%d)\n",
               strings[i].name, strings[i].hz, r.hz, e, r.clarity, r.second_hz, r.second_harmonic);
    }
    printf("strings: worst error %.3f cents\n", worst);
    check("strings within 2 cents (inharmonicity included), no octave errors", worst < 2.0f);
    worst = 0;
    for (size_t i = 0; i < sizeof(strings) / sizeof(strings[0]); i++) {
        tone(strings[i].hz, guitar, 10, 0, 0.8f, 0.002f);
        pitch_analyze(x, &r);
        worst = fmaxf(worst, fabsf(r.hz > 0 ? cents(r.hz, strings[i].hz) : 9999));
    }
    printf("strings without inharmonicity: worst error %.3f cents\n", worst);
    check("harmonic strings within 0.1 cent", worst < 0.1f);

    /* 3. Detuned A4: cents read back. */
    worst = 0;
    for (int c = -45; c <= 45; c += 5) {
        const float f = 440.0f * exp2f(c / 1200.0f);
        tone(f, guitar, 10, 0, 0, 0.002f);
        pitch_analyze(x, &r);
        worst = fmaxf(worst, fabsf(cents(r.hz, 440.0f) - c));
    }
    printf("detuned A4 -45..+45 cents: worst error %.3f cents\n", worst);
    check("detune within 0.05 cent", worst < 0.05f);

    /* 4. Silence and noise: no pitch. */
    for (int i = 0; i < PITCH_N; i++) {
        x[i] = 0;
    }
    pitch_analyze(x, &r);
    printf("silence: hz %.2f rms %.1f dB\n", r.hz, r.rms_db);
    check("silence has no pitch", r.hz == 0);
    for (int i = 0; i < PITCH_N; i++) {
        x[i] = 0.05f * frand();
    }
    pitch_analyze(x, &r);
    printf("white noise: hz %.2f clarity %.3f rms %.1f dB\n", r.hz, r.clarity, r.rms_db);
    check("noise has no pitch", r.hz == 0);

    /* 5. Level: a full-scale sine is 0 dBFS, half scale -6 dB. */
    tone(1000.0f, (const float[]){ 0.5f }, 1, 0, 0, 0);
    pitch_analyze(x, &r);
    printf("0.5 sine at 1 kHz: peak %.3f Hz %.2f dB, rms %.2f dB\n", r.peaks[0].hz, r.peaks[0].db, r.rms_db);
    check("peak level -6 dB", fabsf(r.peaks[0].db + 6.02f) < 0.3f);
    check("peak frequency within 0.01 Hz", fabsf(r.peaks[0].hz - 1000.0f) < 0.01f);

    /* 6. Two notes: A3 and E4. */
    for (int i = 0; i < PITCH_N; i++) {
        const double t = (double)i / PITCH_FS;
        x[i] = (float)(0.3 * sin(2 * M_PI * 220.0 * t) + 0.25 * sin(2 * M_PI * 329.63 * t + 1));
    }
    pitch_analyze(x, &r);
    printf("A3 + E4: hz %.2f clarity %.3f, 2nd %.2f Hz (%.1f dB, H%d)\n", r.hz, r.clarity, r.second_hz,
           r.second_db, r.second_harmonic);
    check("A3+E4 secondary is E4", fabsf(cents(r.second_hz, 329.63f)) < 5.0f);

    /* 7. Bars: a 440 Hz tone lights the bar nearest 440 Hz. */
    tone(440.0f, one, 1, 0, 0, 0.0005f);
    pitch_analyze(x, &r);
    int loudest = 0;
    for (int i = 1; i < PITCH_BARS; i++) {
        if (r.bars_db[i] > r.bars_db[loudest]) {
            loudest = i;
        }
    }
    printf("440 Hz: loudest bar %d (%.1f Hz) at %.1f dB\n", loudest, pitch_bar_hz(loudest), r.bars_db[loudest]);
    check("bar near 440 Hz", fabsf(cents(pitch_bar_hz(loudest), 440.0f)) < 100.0f);

    /* 8. Speed. */
    tone(196.0f, guitar, 10, 0, 0, 0.002f);
    const int reps = 300;
    clock_t c0 = clock();
    for (int i = 0; i < reps; i++) {
        pitch_analyze(x, &r);
    }
    printf("host: %.1f us per analysis\n", 1e6 * (double)(clock() - c0) / CLOCKS_PER_SEC / reps);

    printf(failures ? "\n%d FAILED\n" : "\nall passed\n", failures);
    return failures != 0;
}
