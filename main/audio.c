/*
 * Microphone capture, analysis, and the test tone. See audio.h.
 *
 * I2S0 runs full duplex as master with MCLK = 256 fs, the ratio both codecs
 * are set up for. In full duplex the RX side runs on the TX side's clocks,
 * so both channels stay enabled; TX sends silence (auto-clear) when there's
 * no tone. Not listening, the capture task sleeps and the RX DMA simply
 * overwrites itself; listening again, the stale buffers are drained first.
 */
#include "audio.h"

#include <math.h>
#include <string.h>

#include "board.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "audio";

#define ES7243_ADDR 0x13      /* 7-bit; newer units have an ES7243E at 0x14 */
#define ES7243E_ADDR 0x14
#define MIC_GAIN_DB 30.0f
/* The mic is on the right slot. The left one carries the speaker's output
 * (an echo-cancellation reference): a test tone shows up there as a perfect
 * digital copy, which would fool any "louder slot" heuristic. */
#define MIC_SLOT 1
#define DMA_FRAMES 480        /* 10 ms per DMA buffer */
#define DMA_BUFS 6
#define CAPTURE_CORE 1
#define CAPTURE_PRIO 5
#define LOG_EVERY_MS 500

static i2s_chan_handle_t s_tx, s_rx;
static esp_codec_dev_handle_t s_spk, s_mic;
static TaskHandle_t s_task;
static volatile bool s_listening;
static SemaphoreHandle_t s_tone_lock;

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
/* A recording in progress (audio_record). */
static int16_t *volatile s_rec_dst;
static volatile int s_rec_left;
static SemaphoreHandle_t s_rec_done;
static pitch_result_t s_latest;
static uint32_t s_seq;

static esp_err_t codecs_init(void)
{
    i2c_master_bus_handle_t i2c = board_i2c();
    audio_codec_i2s_cfg_t i2s_cfg = { .port = I2S_NUM_0, .rx_handle = s_rx, .tx_handle = s_tx };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
    audio_codec_i2c_cfg_t dac_i2c = { .port = I2C_NUM_0, .addr = ES8311_CODEC_DEFAULT_ADDR, .bus_handle = i2c };
    const audio_codec_ctrl_if_t *dac_ctrl = audio_codec_new_i2c_ctrl(&dac_i2c);
    /* Seeed probes for the older ES7243 first. */
    const bool es7243 = i2c_master_probe(i2c, ES7243_ADDR, 50) == ESP_OK;
    audio_codec_i2c_cfg_t adc_i2c = {
        .port = I2C_NUM_0,
        .addr = (es7243 ? ES7243_ADDR : ES7243E_ADDR) << 1,
        .bus_handle = i2c,
    };
    const audio_codec_ctrl_if_t *adc_ctrl = audio_codec_new_i2c_ctrl(&adc_i2c);
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    ESP_RETURN_ON_FALSE(data_if && dac_ctrl && adc_ctrl && gpio_if, ESP_ERR_NO_MEM, TAG, "codec interfaces");

    es8311_codec_cfg_t es_cfg = {
        .ctrl_if = dac_ctrl,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = GPIO_NUM_NC,     /* the amp is on an expander rail */
        .use_mclk = true,
        .hw_gain = { .pa_voltage = 5.0, .codec_dac_voltage = 3.3 },
    };
    const audio_codec_if_t *dac = es8311_codec_new(&es_cfg);
    ESP_RETURN_ON_FALSE(dac, ESP_FAIL, TAG, "ES8311 not responding");
    const audio_codec_if_t *adc;
    if (es7243) {
        es7243_codec_cfg_t cfg = { .ctrl_if = adc_ctrl };
        adc = es7243_codec_new(&cfg);
    } else {
        es7243e_codec_cfg_t cfg = { .ctrl_if = adc_ctrl };
        adc = es7243e_codec_new(&cfg);
    }
    ESP_RETURN_ON_FALSE(adc, ESP_FAIL, TAG, "%s not responding", es7243 ? "ES7243" : "ES7243E");

    esp_codec_dev_cfg_t out_cfg = { .dev_type = ESP_CODEC_DEV_TYPE_OUT, .codec_if = dac, .data_if = data_if };
    esp_codec_dev_cfg_t in_cfg = { .dev_type = ESP_CODEC_DEV_TYPE_IN, .codec_if = adc, .data_if = data_if };
    s_spk = esp_codec_dev_new(&out_cfg);
    s_mic = esp_codec_dev_new(&in_cfg);
    ESP_RETURN_ON_FALSE(s_spk && s_mic, ESP_FAIL, TAG, "codec devices");

    esp_codec_dev_sample_info_t fs = { .sample_rate = PITCH_FS, .channel = 2, .bits_per_sample = 16 };
    ESP_RETURN_ON_FALSE(esp_codec_dev_open(s_spk, &fs) == ESP_CODEC_DEV_OK, ESP_FAIL, TAG, "open speaker");
    ESP_RETURN_ON_FALSE(esp_codec_dev_open(s_mic, &fs) == ESP_CODEC_DEV_OK, ESP_FAIL, TAG, "open mic");
    esp_codec_dev_set_in_gain(s_mic, MIC_GAIN_DB);
    esp_codec_dev_set_out_vol(s_spk, 0);
    ESP_LOGI(TAG, "%s mic and ES8311 speaker at %d Hz, mic gain %.0f dB", es7243 ? "ES7243" : "ES7243E", PITCH_FS,
             MIC_GAIN_DB);
    return ESP_OK;
}

/* Reads and drops whatever the RX DMA holds. */
static void drain(int16_t *buf, size_t bytes)
{
    size_t got;
    for (int i = 0; i < DMA_BUFS + 2; i++) {
        if (i2s_channel_read(s_rx, buf, bytes, &got, 0) != ESP_OK || !got) {
            break;
        }
    }
}

static void capture_task(void *arg)
{
    const size_t raw_bytes = AUDIO_HOP * 2 * sizeof(int16_t);
    int16_t *raw = heap_caps_malloc(raw_bytes, MALLOC_CAP_INTERNAL);
    float *ring = heap_caps_malloc(PITCH_N * sizeof(float), MALLOC_CAP_INTERNAL);
    pitch_result_t *res = heap_caps_calloc(1, sizeof(*res), MALLOC_CAP_INTERNAL);
    configASSERT(raw && ring && res);
    int fill = 0;
    int64_t last_log = 0;

    for (;;) {
        if (!s_listening) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            /* A few ms of settling, then drop what piled up while idle. */
            vTaskDelay(pdMS_TO_TICKS(30));
            drain(raw, raw_bytes);
            fill = 0;
            continue;
        }
        size_t got = 0;
        if (i2s_channel_read(s_rx, raw, raw_bytes, &got, pdMS_TO_TICKS(200)) != ESP_OK || got != raw_bytes) {
            ESP_LOGW(TAG, "mic read: %u of %u bytes", (unsigned)got, (unsigned)raw_bytes);
            continue;
        }
        if (s_rec_dst) {
            const int n = s_rec_left < AUDIO_HOP ? s_rec_left : AUDIO_HOP;
            for (int i = 0; i < n; i++) {
                s_rec_dst[i] = raw[2 * i + MIC_SLOT];
            }
            s_rec_dst += n;
            s_rec_left -= n;
            if (!s_rec_left) {
                s_rec_dst = NULL;
                xSemaphoreGive(s_rec_done);
            }
        }
        memmove(ring, ring + AUDIO_HOP, (PITCH_N - AUDIO_HOP) * sizeof(float));
        float *dst = ring + PITCH_N - AUDIO_HOP;
        for (int i = 0; i < AUDIO_HOP; i++) {
            dst[i] = raw[2 * i + MIC_SLOT] * (1.0f / 32768.0f);
        }
        fill += AUDIO_HOP;
        if (fill < PITCH_N) {
            continue;
        }
        fill = PITCH_N;

        const int64_t t0 = esp_timer_get_time();
        pitch_analyze(ring, res);
        res->us = (uint32_t)(esp_timer_get_time() - t0);
        taskENTER_CRITICAL(&s_mux);
        s_latest = *res;
        s_seq++;
        taskEXIT_CRITICAL(&s_mux);

        if (t0 - last_log >= LOG_EVERY_MS * 1000) {
            last_log = t0;
            ESP_LOGI(TAG, "pitch %.2f Hz clarity %.2f level %.1f dB | peak %.1f Hz %.1f dB | 2nd %.1f Hz H%d | %u us",
                     res->hz, res->clarity, res->rms_db, res->n_peaks ? res->peaks[0].hz : 0.0f,
                     res->n_peaks ? res->peaks[0].db : -99.0f, res->second_hz, res->second_harmonic,
                     (unsigned)res->us);
        }
    }
}

esp_err_t audio_init(void)
{
    ESP_RETURN_ON_FALSE(pitch_init(), ESP_ERR_NO_MEM, TAG, "pitch buffers");
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = DMA_BUFS;
    chan_cfg.dma_frame_num = DMA_FRAMES;
    chan_cfg.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_tx, &s_rx), TAG, "i2s channels");
    const i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(PITCH_FS),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = BOARD_I2S_MCLK,
            .bclk = BOARD_I2S_BCLK,
            .ws = BOARD_I2S_WS,
            .dout = BOARD_I2S_DOUT,
            .din = BOARD_I2S_DIN,
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx, &std_cfg), TAG, "i2s tx");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_rx, &std_cfg), TAG, "i2s rx");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx), TAG, "i2s tx on");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_rx), TAG, "i2s rx on");
    ESP_RETURN_ON_ERROR(codecs_init(), TAG, "codecs");

    s_tone_lock = xSemaphoreCreateMutex();
    s_rec_done = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(s_tone_lock && s_rec_done, ESP_ERR_NO_MEM, TAG, "locks");
    ESP_RETURN_ON_FALSE(xTaskCreatePinnedToCore(capture_task, "capture", 6144, NULL, CAPTURE_PRIO, &s_task,
                                                CAPTURE_CORE) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "capture task");
    return ESP_OK;
}

void audio_set_listening(bool on)
{
    if (on == s_listening) {
        return;
    }
    if (on) {
        taskENTER_CRITICAL(&s_mux);
        s_seq = 0;
        taskEXIT_CRITICAL(&s_mux);
    }
    s_listening = on;
    if (on) {
        xTaskNotifyGive(s_task);
    }
    ESP_LOGI(TAG, "listening %s", on ? "on" : "off");
}

bool audio_listening(void)
{
    return s_listening;
}

uint32_t audio_latest(pitch_result_t *out)
{
    taskENTER_CRITICAL(&s_mux);
    const uint32_t seq = s_seq;
    if (seq) {
        *out = s_latest;
    }
    taskEXIT_CRITICAL(&s_mux);
    return seq;
}

esp_err_t audio_record(int16_t *dst, int n, int timeout_ms)
{
    ESP_RETURN_ON_FALSE(s_listening && n > 0, ESP_ERR_INVALID_STATE, TAG, "not listening");
    xSemaphoreTake(s_rec_done, 0);
    s_rec_left = n;
    s_rec_dst = dst;
    if (xSemaphoreTake(s_rec_done, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        s_rec_dst = NULL;
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t audio_tone(float hz, int ms, int volume)
{
    enum { CHUNK = 480 };
    static int16_t buf[CHUNK * 2];
    xSemaphoreTake(s_tone_lock, portMAX_DELAY);
    board_speaker_amp(true);
    esp_codec_dev_set_out_vol(s_spk, volume);
    const int total = PITCH_FS * ms / 1000;
    const int fade = PITCH_FS / 100;       /* 10 ms in and out: no clicks */
    const float w = 2.0f * (float)M_PI * hz / PITCH_FS;
    float phase = 0;
    esp_err_t err = ESP_OK;
    for (int n = 0; n < total && err == ESP_OK; n += CHUNK) {
        for (int i = 0; i < CHUNK; i++) {
            const int k = n + i;
            float g = 0.5f;
            if (k < fade) {
                g *= (float)k / fade;
            } else if (k > total - fade) {
                g *= k < total ? (float)(total - k) / fade : 0.0f;
            }
            const int16_t v = (int16_t)(32767.0f * g * sinf(phase));
            phase += w;
            if (phase > 2.0f * (float)M_PI) {
                phase -= 2.0f * (float)M_PI;
            }
            buf[2 * i] = v;
            buf[2 * i + 1] = v;
        }
        size_t done;
        err = i2s_channel_write(s_tx, buf, sizeof(buf), &done, pdMS_TO_TICKS(500));
    }
    /* Let the DMA play out before the amp goes off. */
    vTaskDelay(pdMS_TO_TICKS(DMA_BUFS * DMA_FRAMES * 1000 / PITCH_FS + 20));
    esp_codec_dev_set_out_vol(s_spk, 0);
    board_speaker_amp(false);
    xSemaphoreGive(s_tone_lock);
    return err;
}
