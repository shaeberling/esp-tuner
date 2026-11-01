#pragma once
/*
 * The one FFT the pitch code needs: complex, forward, in place, natural
 * order out. On the ESP32-S3 it's esp-dsp's radix-2 FFT, which uses the
 * S3's SIMD instructions; on the host (tests) a plain radix-2 one.
 */
#include <stdbool.h>

/* Prepares tables for transforms of up to max_n complex points (power of 2). */
bool fft_init(int max_n);

/* data holds n complex values as re, im pairs; n is a power of 2. */
void fft_complex(float *data, int n);
