/*
 * Runs main/pitch.c over a 16-bit mono WAV the way the firmware does
 * (PITCH_N blocks every 1024 samples) and prints each analysis:
 *   ./analyze_wav FILE.wav [expected_hz]
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pitch.h"

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s FILE.wav [expected_hz]\n", argv[0]);
        return 2;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) {
        perror(argv[1]);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 44, SEEK_SET);
    int n = (int)((size - 44) / 2);
    short *pcm = malloc(n * sizeof(short));
    if (fread(pcm, sizeof(short), n, f) != (size_t)n) {
        return 1;
    }
    fclose(f);
    const float want = argc > 2 ? strtof(argv[2], NULL) : 0;
    /* Experiments: MIX_HZ/MIX_DB add a sine; HPF_HZ high-passes (2nd-order Butterworth). */
    const char *mix_hz = getenv("MIX_HZ"), *mix_db = getenv("MIX_DB"), *hpf = getenv("HPF_HZ");
    float *xf = malloc(n * sizeof(float));
    for (int i = 0; i < n; i++) {
        xf[i] = pcm[i] / 32768.0f;
        if (mix_hz) {
            xf[i] += powf(10, strtof(mix_db, NULL) / 20) * sinf(2 * (float)M_PI * strtof(mix_hz, NULL) * i / PITCH_FS);
        }
    }
    if (hpf) {
        const double w = tan(M_PI * strtod(hpf, NULL) / PITCH_FS), q = M_SQRT1_2;
        const double k = 1 / (1 + w / q + w * w);
        const double b0 = k, b1 = -2 * k, b2 = k, a1 = 2 * (w * w - 1) * k, a2 = (1 - w / q + w * w) * k;
        double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
        for (int i = 0; i < n; i++) {
            const double y = b0 * xf[i] + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
            x2 = x1; x1 = xf[i]; y2 = y1; y1 = y;
            xf[i] = (float)y;
        }
    }
    pitch_init();
    static float x[PITCH_N];
    pitch_result_t r;
    int count = 0, hits = 0;
    double sum = 0, sum2 = 0;
    for (int at = 0; at + PITCH_N <= n; at += 1024) {
        for (int i = 0; i < PITCH_N; i++) {
            x[i] = xf[at + i];
        }
        pitch_analyze(x, &r);
        const float c = want > 0 && r.hz > 0 ? 1200 * log2f(r.hz / want) : 0;
        printf("%6d  %8.2f Hz  %+7.2f c  clarity %.2f  level %6.1f  peaks", at, r.hz, c, r.clarity, r.rms_db);
        for (int i = 0; i < r.n_peaks && i < 4; i++) {
            printf("  %.1f/%.0f", r.peaks[i].hz, r.peaks[i].db);
        }
        printf("\n");
        count++;
        if (r.hz > 0 && r.clarity >= 0.8f) {
            hits++;
            sum += c;
            sum2 += c * c;
        }
    }
    if (want > 0 && hits) {
        const double m = sum / hits;
        printf("%d/%d blocks with pitch; error %+.2f cents mean, %.2f sd\n", hits, count, m, sqrt(sum2 / hits - m * m));
    }
    return 0;
}
