/* fft.h on the host: a plain iterative radix-2 FFT, for the tests. */
#include <math.h>
#include <stdlib.h>

#include "fft.h"

static float *s_tw;   /* exp(-2 pi i k / max_n), k < max_n / 2 */
static int s_max;

bool fft_init(int max_n)
{
    s_max = max_n;
    s_tw = malloc(sizeof(float) * max_n);
    for (int k = 0; k < max_n / 2; k++) {
        s_tw[2 * k] = (float)cos(2 * M_PI * k / max_n);
        s_tw[2 * k + 1] = (float)-sin(2 * M_PI * k / max_n);
    }
    return s_tw != NULL;
}

void fft_complex(float *d, int n)
{
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) {
            j ^= bit;
        }
        j ^= bit;
        if (i < j) {
            float t = d[2 * i]; d[2 * i] = d[2 * j]; d[2 * j] = t;
            t = d[2 * i + 1]; d[2 * i + 1] = d[2 * j + 1]; d[2 * j + 1] = t;
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        const int step = s_max / len;
        for (int i = 0; i < n; i += len) {
            for (int k = 0; k < len / 2; k++) {
                const float wr = s_tw[2 * k * step], wi = s_tw[2 * k * step + 1];
                float *a = &d[2 * (i + k)], *b = &d[2 * (i + k + len / 2)];
                const float tr = b[0] * wr - b[1] * wi, ti = b[0] * wi + b[1] * wr;
                b[0] = a[0] - tr; b[1] = a[1] - ti;
                a[0] += tr; a[1] += ti;
            }
        }
    }
}
