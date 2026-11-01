/*
 * McLeod Pitch Method with FFT autocorrelation, plus a Hann spectrum for the
 * histogram and the secondary peak. See pitch.h.
 *
 * Per block, two real FFTs of M = 2N points (each a packed N-point complex
 * FFT):
 *   1. the zero-padded block -> X[k]. Its Hann-windowed spectrum comes from
 *      X directly: a Hann window of period N multiplies by
 *      0.5 - 0.5 cos(2 pi n / N), which on the M-point grid is
 *      0.5 X[k] - 0.25 (X[k-2] + X[k+2]): no third FFT.
 *   2. |X[k]|^2 -> the autocorrelation r(tau) (Wiener-Khinchin), for the NSDF.
 *
 * Buffers, all floats:
 *   s_buf   M        real FFT in/out (packed as M/2 complex)
 *   s_pow   M/2 + 1  power spectrum
 *   s_spec  K_MAX+3  Hann spectrum, dBFS, up to SPEC_MAX_HZ
 *   s_nsdf  TAU_MAX + 2
 *   s_tw    M/2 + 2  twiddles for the real-FFT post-processing
 */
#include "pitch.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "fft.h"

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
/* Internal RAM when there's room: the FFT runs several times faster there than in PSRAM. */
static void *alloc_floats(size_t n)
{
    void *p = heap_caps_aligned_alloc(16, n * sizeof(float), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    return p ? p : heap_caps_aligned_alloc(16, n * sizeof(float), MALLOC_CAP_SPIRAM);
}
#else
static void *alloc_floats(size_t n)
{
    return aligned_alloc(16, ((n * sizeof(float) + 15) / 16) * 16);
}
#endif

#define N PITCH_N
#define M (2 * PITCH_N)                 /* zero-padded: linear, not circular, correlation */
#define H (M / 2)                       /* complex points in the packed FFT */
#define DF ((float)PITCH_FS / M)        /* spectrum bin spacing: 5.86 Hz */
#define TAU_MIN ((int)(PITCH_FS / PITCH_MAX_HZ))
#define TAU_MAX ((int)(PITCH_FS / PITCH_MIN_HZ) + 1)
#define SPEC_MAX_HZ 4200.0f             /* peaks and bars look no higher */
#define K_MAX ((int)(SPEC_MAX_HZ / DF) + 2)

/* McLeod's "k": the first NSDF key maximum within this fraction of the
 * highest one is the period. Higher picks longer periods (octave-down
 * errors), lower picks shorter ones (octave-up). */
#define MPM_CUTOFF 0.93f
#define MPM_MIN_CLARITY 0.60f
#define MIN_RMS_DB -62.0f               /* quieter blocks have no pitch */
#define PEAK_FLOOR_DB -78.0f
#define PEAK_RANGE_DB 30.0f             /* peaks this far under the strongest are ignored */
#define PEAK_MIN_SEP 6                  /* bins: closer to a stronger peak is its side lobe */
#define UNSUPPORTED_DB 30.0f            /* a fundamental this far under the strongest peak isn't there */

static float *s_buf, *s_pow, *s_spec, *s_nsdf, *s_tw;
static float s_bar_hz[PITCH_BARS];

bool pitch_init(void)
{
    if (s_buf) {
        return true;
    }
    s_buf = alloc_floats(M);
    s_pow = alloc_floats(H + 1);
    s_spec = alloc_floats(K_MAX + 3);
    s_nsdf = alloc_floats(TAU_MAX + 2);
    s_tw = alloc_floats(H + 2);
    if (!s_buf || !s_pow || !s_spec || !s_nsdf || !s_tw || !fft_init(H)) {
        return false;
    }
    /* W^k = exp(-2 pi i k / M), k = 0 .. H/2 */
    for (int k = 0; k <= H / 2; k++) {
        const float a = 2.0f * (float)M_PI * k / M;
        s_tw[2 * k] = cosf(a);
        s_tw[2 * k + 1] = -sinf(a);
    }
    for (int i = 0; i < PITCH_BARS; i++) {
        s_bar_hz[i] = PITCH_BAR_LO_HZ * exp2f((float)i / PITCH_BARS_PER_OCT);
    }
    return true;
}

float pitch_bar_hz(int i)
{
    return s_bar_hz[i];
}

/*
 * FFT of M real values in s_buf, in place. Out, packed: s_buf[0] = X[0],
 * s_buf[1] = X[M/2] (both real), s_buf[2k], s_buf[2k+1] = X[k] for
 * 0 < k < M/2. The M reals are read as M/2 complex values (even samples
 * real, odd imaginary), transformed, and the two interleaved spectra
 * separated: X[k] = E[k] + W^k O[k].
 */
static void rfft(void)
{
    float *z = s_buf;
    fft_complex(z, H);
    const float z0r = z[0], z0i = z[1];
    z[0] = z0r + z0i;
    z[1] = z0r - z0i;
    for (int k = 1; k <= H / 2; k++) {
        const int m = H - k;
        const float ar = z[2 * k], ai = z[2 * k + 1];
        const float br = z[2 * m], bi = z[2 * m + 1];
        /* E = (Z[k] + conj Z[m]) / 2, O = (Z[k] - conj Z[m]) / 2i */
        const float er = 0.5f * (ar + br), ei = 0.5f * (ai - bi);
        const float or_ = 0.5f * (ai + bi), oi = -0.5f * (ar - br);
        const float wr = s_tw[2 * k], wi = s_tw[2 * k + 1];
        const float tr = wr * or_ - wi * oi, ti = wr * oi + wi * or_;
        z[2 * k] = er + tr;              /* X[k] = E + W^k O */
        z[2 * k + 1] = ei + ti;
        if (m != k) {
            z[2 * m] = er - tr;          /* X[M/2 - k] = conj(E - W^k O) */
            z[2 * m + 1] = -(ei - ti);
        }
    }
}

/* X[k] from the packed s_buf, for -M/2 < k < M/2 (X[-k] = conj X[k]). */
static inline void bin(int k, float *re, float *im)
{
    if (k == 0) {
        *re = s_buf[0];
        *im = 0.0f;
    } else if (k > 0) {
        *re = s_buf[2 * k];
        *im = s_buf[2 * k + 1];
    } else {
        *re = s_buf[-2 * k];
        *im = -s_buf[-2 * k + 1];
    }
}

/* Parabola through (-1, a), (0, b), (1, c): the vertex's offset and height. */
static inline float parabola(float a, float b, float c, float *peak)
{
    const float d = a - 2.0f * b + c;
    const float p = d != 0.0f ? 0.5f * (a - c) / d : 0.0f;
    if (peak) {
        *peak = b - 0.25f * (a - c) * p;
    }
    return p;
}

/*
 * A peak in the Hann dB spectrum: the parabola's vertex, less the bias that
 * a parabola has on a Hann main lobe (up to 0.0033 bins, ~0.5 cents at
 * 60 Hz). The correction is a least-squares fit of the bias against the
 * offset for this window and 2x zero padding; it leaves 0.000003 bins.
 */
static inline float spec_peak(int k, float *db)
{
    const float p = parabola(s_spec[k - 1], s_spec[k], s_spec[k + 1], db);
    const float q = p * (1.0f - 4.0f * p * p);
    return k + p - 0.016583f * q - 0.003686f * q * p * p;
}

/* Hann spectrum in dBFS into s_spec[0 .. K_MAX + 1] from X in s_buf: a
 * full-scale sine reads 0 dB. A sine of amplitude A peaks at
 * A * sum(w) / 2 = A * N / 4. */
static void hann_spectrum(void)
{
    const float norm = 4.0f / N;
    const float norm2 = norm * norm;
    for (int k = 1; k <= K_MAX + 1; k++) {
        float r0, i0, r1, i1, r2, i2;
        bin(k, &r0, &i0);
        bin(k - 2, &r1, &i1);
        bin(k + 2, &r2, &i2);
        const float re = 0.5f * r0 - 0.25f * (r1 + r2);
        const float im = 0.5f * i0 - 0.25f * (i1 + i2);
        s_spec[k] = 10.0f * log10f((re * re + im * im) * norm2 + 1e-12f);
    }
    s_spec[0] = s_spec[1];
}

/*
 * NSDF n(tau) = 2 r(tau) / m(tau), r the autocorrelation and
 * m(tau) = sum over the overlap of x[j]^2 + x[j + tau]^2, from X in s_buf.
 * Its key maxima (McLeod): the highest point of each positive lobe after
 * the first zero crossing, interpolated, into s_key_tau / s_key_val.
 * Returns their count.
 */
enum { MAX_KEYS = 64 };
static float s_key_tau[MAX_KEYS], s_key_val[MAX_KEYS];

static int nsdf_keys(const float *x, float mean)
{
    s_pow[0] = s_buf[0] * s_buf[0];
    s_pow[H] = s_buf[1] * s_buf[1];
    for (int k = 1; k < H; k++) {
        s_pow[k] = s_buf[2 * k] * s_buf[2 * k] + s_buf[2 * k + 1] * s_buf[2 * k + 1];
    }
    /* Autocorrelation: the transform of the (real, even) power spectrum. */
    s_buf[0] = s_pow[0];
    for (int k = 1; k < H; k++) {
        s_buf[k] = s_pow[k];
        s_buf[M - k] = s_pow[k];
    }
    s_buf[H] = s_pow[H];
    rfft();
    /* r(tau) * M is s_buf[2 tau]; m(tau) runs down from m(0) = 2 r(0). */
    const float r0 = s_buf[0];
    if (r0 <= 0.0f) {
        return 0;
    }
    float m = 2.0f * r0;
    s_nsdf[0] = 1.0f;
    for (int t = 1; t <= TAU_MAX + 1; t++) {
        const float a = x[t - 1] - mean, b = x[N - t] - mean;
        m -= (a * a + b * b) * M;           /* r is scaled by M, so m is too */
        s_nsdf[t] = m > 0.0f ? 2.0f * s_buf[2 * t] / m : 0.0f;
    }

    int n_keys = 0;
    int t = 1;
    while (t <= TAU_MAX && s_nsdf[t] > 0.0f) {
        t++;
    }
    int best = 0;
    for (; t <= TAU_MAX; t++) {
        if (s_nsdf[t] > 0.0f && s_nsdf[t] > s_nsdf[t - 1] && s_nsdf[t] >= s_nsdf[t + 1]) {
            if (!best || s_nsdf[t] > s_nsdf[best]) {
                best = t;
            }
        }
        if (best && (s_nsdf[t + 1] <= 0.0f || t == TAU_MAX)) {
            if (best >= TAU_MIN && n_keys < MAX_KEYS) {
                float v;
                const float p = parabola(s_nsdf[best - 1], s_nsdf[best], s_nsdf[best + 1], &v);
                s_key_tau[n_keys] = best + p;
                s_key_val[n_keys] = v;
                n_keys++;
            }
            best = 0;
        }
    }
    return n_keys;
}

/* McLeod's choice: the first key maximum within MPM_CUTOFF of the highest. */
static float mpm_pick(int n_keys, float *clarity)
{
    float top = 0.0f;
    for (int i = 0; i < n_keys; i++) {
        top = fmaxf(top, s_key_val[i]);
    }
    *clarity = top;
    if (top < MPM_MIN_CLARITY) {
        return 0.0f;
    }
    for (int i = 0; i < n_keys; i++) {
        if (s_key_val[i] >= MPM_CUTOFF * top) {
            *clarity = s_key_val[i];
            return (float)PITCH_FS / s_key_tau[i];
        }
    }
    return 0.0f;
}

/* Spectral peaks, strongest first, ignoring side lobes of stronger ones. */
static int find_peaks(pitch_peak_t *out, int max_out)
{
    const int k_lo = (int)(PITCH_MIN_HZ / DF);
    float top = -200.0f;
    for (int k = k_lo; k < K_MAX; k++) {
        top = fmaxf(top, s_spec[k]);
    }
    const float floor_db = fmaxf(PEAK_FLOOR_DB, top - PEAK_RANGE_DB);
    /* Candidates, kept sorted by level. */
    enum { MAX_CAND = 32 };
    int ck[MAX_CAND];
    int nc = 0;
    for (int k = k_lo; k < K_MAX; k++) {
        const float v = s_spec[k];
        if (v < floor_db || v <= s_spec[k - 1] || v < s_spec[k + 1]) {
            continue;
        }
        int pos = nc;
        while (pos > 0 && s_spec[ck[pos - 1]] < v) {
            pos--;
        }
        if (pos >= MAX_CAND) {
            continue;
        }
        if (nc < MAX_CAND) {
            nc++;
        }
        memmove(&ck[pos + 1], &ck[pos], (nc - 1 - pos) * sizeof(int));
        ck[pos] = k;
    }
    int n = 0;
    for (int i = 0; i < nc && n < max_out; i++) {
        bool lobe = false;
        for (int j = 0; j < i; j++) {
            if (abs(ck[i] - ck[j]) < PEAK_MIN_SEP) {
                lobe = true;
                break;
            }
        }
        if (lobe) {
            continue;
        }
        const int k = ck[i];
        float db;
        out[n].hz = spec_peak(k, &db) * DF;
        out[n].db = db;
        n++;
    }
    return n;
}

/* Each bar: the loudest bin in its 1/8 octave, or the level at its center
 * where the bar is narrower than a bin. */
static void bars(float *bars_db)
{
    const float half = exp2f(0.5f / PITCH_BARS_PER_OCT);
    for (int i = 0; i < PITCH_BARS; i++) {
        const float fc = s_bar_hz[i];
        int k0 = (int)ceilf(fc / half / DF);
        int k1 = (int)floorf(fc * half / DF);
        if (k1 > K_MAX) {
            k1 = K_MAX;
        }
        float v;
        if (k1 < k0) {
            const float kf = fc / DF;
            const int k = (int)kf;
            const float f = kf - k;
            v = s_spec[k] * (1.0f - f) + s_spec[k + 1] * f;
        } else {
            v = s_spec[k0];
            for (int k = k0 + 1; k <= k1; k++) {
                v = fmaxf(v, s_spec[k]);
            }
        }
        bars_db[i] = v;
    }
}

/* The Hann spectrum's loudest bin within two bins of hz. */
static float level_near(float hz)
{
    const int k = (int)lroundf(hz / DF);
    float v = -200.0f;
    for (int i = k - 2; i <= k + 2; i++) {
        if (i >= 1 && i <= K_MAX) {
            v = fmaxf(v, s_spec[i]);
        }
    }
    return v;
}

static float cents_between(float a, float b)
{
    return 1200.0f * log2f(a / b);
}

/*
 * The NSDF finds the period robustly but integrates noise from the whole
 * band: on the Watcher's mic, room noise 8-16 dB under a tone pulled it up
 * to +17 cents. A spectral peak only sees the noise inside its own main lobe
 * (about +-12 Hz), so the frequency is refined from the peaks of the
 * fundamental and its first harmonics, near where the NSDF put them,
 * weighted by their power. Without a usable peak, or if they disagree with
 * the NSDF by more than REFINE_MAX_CENTS, the NSDF's value stands.
 */
#define REFINE_HARMONICS 6
#define REFINE_MIN_HZ 60.0f      /* below, harmonics' main lobes overlap */
#define REFINE_RANGE_DB 35.0f    /* harmonics this far under the strongest don't count */
#define REFINE_MAX_CENTS 40.0f

static float refine_f0(float f0, float top_db)
{
    if (f0 < REFINE_MIN_HZ) {
        return f0;
    }
    const float spacing = f0 / DF;                 /* bins between harmonics */
    float sum_w = 0.0f, sum_f = 0.0f;
    for (int h = 1; h <= REFINE_HARMONICS && h * f0 < SPEC_MAX_HZ - 50.0f; h++) {
        const float center = h * f0 / DF;
        float half = 2.0f + 0.012f * center;       /* the NSDF's error, scaled up */
        if (half > 0.5f * spacing - 1.0f) {
            half = 0.5f * spacing - 1.0f;
        }
        const int k0 = (int)fmaxf(2.0f, ceilf(center - half));
        const int k1 = (int)fminf((float)K_MAX - 1, floorf(center + half));
        int best = -1;
        for (int k = k0; k <= k1; k++) {
            if (s_spec[k] > s_spec[k - 1] && s_spec[k] >= s_spec[k + 1] && (best < 0 || s_spec[k] > s_spec[best])) {
                best = k;
            }
        }
        if (best < 0 || s_spec[best] < top_db - REFINE_RANGE_DB || s_spec[best] < PEAK_FLOOR_DB) {
            continue;
        }
        float db;
        const float bin = spec_peak(best, &db);
        const float w = exp2f(0.33219281f * db);   /* 10^(db / 10) */
        sum_w += w;
        sum_f += w * bin * DF / h;
    }
    if (sum_w <= 0.0f) {
        return f0;
    }
    const float f = sum_f / sum_w;
    return fabsf(cents_between(f, f0)) <= REFINE_MAX_CENTS ? f : f0;
}

void pitch_analyze(const float *x, pitch_result_t *out)
{
    float sum = 0.0f;
    for (int i = 0; i < N; i++) {
        sum += x[i];
    }
    const float mean = sum / N;
    float sum2 = 0.0f;
    for (int i = 0; i < N; i++) {
        const float v = x[i] - mean;
        s_buf[i] = v;
        sum2 += v * v;
    }
    memset(s_buf + N, 0, (M - N) * sizeof(float));
    const float rms = sqrtf(sum2 / N);
    /* A full-scale sine's RMS is 1/sqrt(2): call that 0 dBFS, like the peaks. */
    out->rms_db = 20.0f * log10f(rms * (float)M_SQRT2 + 1e-9f);

    rfft();
    hann_spectrum();
    out->n_peaks = find_peaks(out->peaks, PITCH_MAX_PEAKS);
    bars(out->bars_db);

    const int n_keys = out->rms_db >= MIN_RMS_DB ? nsdf_keys(x, mean) : 0;
    out->hz = mpm_pick(n_keys, &out->clarity);
    /* A period with nothing at its frequency is a virtual pitch: two notes a
     * fifth apart repeat at an octave under the lower one. A tuner wants the
     * notes played, so take the strongest real peak instead. A string's weak
     * fundamental is still well above this. */
    if (out->hz > 0.0f && out->n_peaks && level_near(out->hz) < out->peaks[0].db - UNSUPPORTED_DB) {
        out->hz = out->peaks[0].hz;
    } else if (out->hz > 0.0f && out->n_peaks) {
        out->hz = refine_f0(out->hz, out->peaks[0].db);
    }

    /* Secondary: the strongest peak more than a third of a semitone away
     * from the fundamental (or, without one, the second strongest peak). */
    out->second_hz = 0.0f;
    out->second_db = 0.0f;
    out->second_harmonic = 0;
    const float ref = out->hz > 0.0f ? out->hz : (out->n_peaks ? out->peaks[0].hz : 0.0f);
    if (ref > 0.0f) {
        for (int i = 0; i < out->n_peaks; i++) {
            if (fabsf(cents_between(out->peaks[i].hz, ref)) > 35.0f) {
                out->second_hz = out->peaks[i].hz;
                out->second_db = out->peaks[i].db;
                break;
            }
        }
    }
    if (out->second_hz > 0.0f && out->hz > 0.0f) {
        const float n = roundf(out->second_hz / out->hz);
        if (n >= 2.0f && n <= 16.0f && fabsf(cents_between(out->second_hz, n * out->hz)) < 30.0f) {
            out->second_harmonic = (int)n;
        }
    }
}

#ifdef ESP_PLATFORM
#include <stdio.h>

#include "esp_timer.h"

void pitch_bench(void)
{
    static float x[N];
    for (int i = 0; i < N; i++) {
        x[i] = 0.3f * sinf(2.0f * (float)M_PI * 196.0f * i / PITCH_FS) +
               0.6f * sinf(2.0f * (float)M_PI * 392.0f * i / PITCH_FS + 1.0f);
    }
    for (int i = 0; i < M; i++) {
        s_buf[i] = 0.0f;
    }
    int64_t t0 = esp_timer_get_time();
    for (int i = 0; i < 20; i++) {
        fft_complex(s_buf, H);
    }
    int64_t t1 = esp_timer_get_time();
    for (int i = 0; i < 20; i++) {
        rfft();
    }
    int64_t t2 = esp_timer_get_time();
    pitch_result_t r;
    for (int i = 0; i < 20; i++) {
        pitch_analyze(x, &r);
    }
    int64_t t3 = esp_timer_get_time();
    printf("bench: %d-point complex FFT %lld us, real %d-point FFT %lld us, full analysis %lld us -> %.2f Hz\n",
           H, (t1 - t0) / 20, M, (t2 - t1) / 20, (t3 - t2) / 20, r.hz);
}
#endif
