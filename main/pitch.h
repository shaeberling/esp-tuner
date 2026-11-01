#pragma once
/*
 * Pitch and spectrum of one block of audio.
 *
 * The fundamental comes from the McLeod Pitch Method (normalized square
 * difference function, NSDF): robust against the octave errors plain FFT
 * peak picking makes on strings whose 2nd harmonic is louder than the
 * fundamental. Its autocorrelation is computed with FFTs (Wiener-Khinchin),
 * so a 4096-sample block costs three 4096-point complex FFTs instead of the
 * ~5 M multiply-adds of the direct sum. Real-input FFTs are packed into
 * half-size complex ones.
 *
 * A Hann-windowed spectrum of the same block gives the histogram bars and
 * the strongest spectral peaks, from which the secondary frequency is
 * picked.
 */
#include <stdbool.h>
#include <stdint.h>

#define PITCH_FS 48000            /* sample rate, Hz */
#define PITCH_N 4096              /* block length: 85 ms at 48 kHz */
#define PITCH_MIN_HZ 38.0f        /* a bit under E1 (41.2 Hz), the bass's low string */
#define PITCH_MAX_HZ 2100.0f      /* a bit over C7 */
#define PITCH_BARS 48             /* histogram bars, 1/8 octave each ... */
#define PITCH_BARS_PER_OCT 8
#define PITCH_BAR_LO_HZ 55.0f     /* ... from A1 up six octaves; each A is a bar's center */
#define PITCH_MAX_PEAKS 8

typedef struct {
    float hz;      /* interpolated frequency */
    float db;      /* level, dBFS (a full-scale sine is 0 dB) */
} pitch_peak_t;

typedef struct {
    /* The fundamental: 0 when there's no clear pitch. */
    float hz;
    float clarity;          /* NSDF peak, 0..1: how periodic the block is */
    float rms_db;           /* block level, dBFS */
    /* Spectral peaks, strongest first. */
    pitch_peak_t peaks[PITCH_MAX_PEAKS];
    int n_peaks;
    /* The secondary frequency: the strongest peak that isn't the
     * fundamental itself. 0 when there's none worth showing. */
    float second_hz;
    float second_db;
    int second_harmonic;    /* n when second_hz is the fundamental's n-th harmonic, else 0 */
    /* Histogram: each bar's loudest bin, dBFS. */
    float bars_db[PITCH_BARS];
    /* Time the analysis took, microseconds (set by the caller if it wants). */
    uint32_t us;
} pitch_result_t;

/* Allocates the work buffers and FFT tables. */
bool pitch_init(void);

/* Analyzes PITCH_N samples in -1..1. */
void pitch_analyze(const float *x, pitch_result_t *out);

/* Center frequency of histogram bar i. */
float pitch_bar_hz(int i);

/* Times the FFT and a full analysis, prints the result (device only). */
void pitch_bench(void);
