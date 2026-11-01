/*
 * fft.h on the ESP32-S3: esp-dsp's Xtensa-optimized complex FFTs.
 *
 * Radix-4 when the size is a power of 4 (the analysis' 4096 is): measured
 * on the Watcher inside the analysis, 1.42 ms against radix-2's 2.04 ms
 * for 4096 points, with the same result to ~3e-7. Its 64 KB twiddle table
 * sits in PSRAM, which the data cache hides.
 */
#include "fft.h"

#include "dsps_fft2r.h"
#include "dsps_fft4r.h"
#include "esp_log.h"

static const char *TAG = "fft";
static bool s_radix4;

static bool power_of_4(int n)
{
    return n > 0 && !(n & (n - 1)) && (n & 0x55555555);
}

bool fft_init(int max_n)
{
    s_radix4 = power_of_4(max_n);
    esp_err_t err = s_radix4 ? dsps_fft4r_init_fc32(NULL, max_n) : dsps_fft2r_init_fc32(NULL, max_n);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "FFT init (%d points): %s", max_n, esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "radix-%d, up to %d points", s_radix4 ? 4 : 2, max_n);
    return true;
}

void fft_complex(float *data, int n)
{
    if (s_radix4) {
        dsps_fft4r_fc32(data, n);
        dsps_bit_rev4r_fc32(data, n);
    } else {
        dsps_fft2r_fc32(data, n);
        dsps_bit_rev2r_fc32(data, n);
    }
}
