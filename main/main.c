/*
 * SenseCAP Watcher chromatic tuner.
 *
 * Push the wheel (or tap the screen) to listen; it stops by itself after
 * AUTO_STOP_MS without a clear pitch, or on the next push. Turn the wheel to
 * move the reference A4 (415..466 Hz, kept across restarts; it stops at 440 on
 * the way through, and shows amber anywhere else). Hold the wheel to power off.
 *
 * The serial console (115200 baud) takes a few commands for testing:
 *   tone <hz> [ms] [volume]   play a sine on the speaker (and listen)
 *   sweep                     play a series of tones, log what was heard
 *   listen | stop             start or stop listening
 *   a4 <hz>                   set the reference
 *   snap                      print the screen (tools/snap.py makes a PNG)
 *   stat                      memory and timing
 *   bench                     time the FFT and the analysis
 *   rec [n] [tone_hz]         dump n raw mic samples (tools/rec.py), optionally over a tone
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio.h"
#include "board.h"
#include "display.h"
#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "note.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "ui.h"

static const char *TAG = "tuner";

#define POLL_MS 20
#define DEBOUNCE 3
#define HOLD_HINT_MS 1000       /* holding the wheel this long shows the power-off hint ... */
#define HOLD_OFF_MS 2500        /* ... and this long powers off */
#define AUTO_STOP_MS 20000
#define POWER_POLL_MS 5000
#define COUNTS_PER_STEP 2       /* encoder edges per wheel step */
#define PUSH_QUIET_MS 300       /* turns while the wheel is pushed, or just after, are the push wobbling it */
#define A4_MIN 415
#define A4_MAX 466
#define A4_STD 440
#define A4_DETENT_MS 400        /* reaching 440, the wheel has to rest this long before it moves on */
#define A4_TOAST_MS 1500
#define A4_SAVE_MS 3000
#define BRIGHTNESS 85

static int s_a4 = 440;

static int load_a4(void)
{
    nvs_handle_t h;
    int32_t v = 440;
    if (nvs_open("tuner", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_i32(h, "a4", &v);
        nvs_close(h);
    }
    return v >= A4_MIN && v <= A4_MAX ? (int)v : 440;
}

static void save_a4(int a4)
{
    nvs_handle_t h;
    if (nvs_open("tuner", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, "a4", a4);
        nvs_commit(h);
        nvs_close(h);
        ESP_LOGI(TAG, "A4 = %d Hz saved", a4);
    }
}

static void set_listening(bool on)
{
    /* The UI first: it restarts the auto-stop clock before audio_listening() turns true. */
    ui_set_listening(on);
    audio_set_listening(on);
}

static void set_a4(int a4)
{
    s_a4 = a4 < A4_MIN ? A4_MIN : a4 > A4_MAX ? A4_MAX : a4;
    ui_set_a4(s_a4);
}

/* ---- console ------------------------------------------------------------------------ */

typedef struct {
    float hz;
    int ms, vol;
} tone_req_t;

static void tone_task(void *arg)
{
    tone_req_t *t = arg;
    audio_tone(t->hz, t->ms, t->vol);
    free(t);
    vTaskDelete(NULL);
}

static void tone_async(float hz, int ms, int vol)
{
    tone_req_t *t = malloc(sizeof(*t));
    if (!t) {
        return;
    }
    *t = (tone_req_t){ hz, ms, vol };
    if (xTaskCreate(tone_task, "tone", 4096, t, 4, NULL) != pdPASS) {
        free(t);
    }
}

static void sweep(void)
{
    static const float freqs[] = { 196.0f, 246.94f, 329.63f, 440.0f, 523.25f, 659.26f, 880.0f, 1046.5f, 1318.5f };
    if (!audio_listening()) {
        set_listening(true);
    }
    printf("sweep: played -> heard (cents), clarity, level\n");
    for (size_t i = 0; i < sizeof(freqs) / sizeof(freqs[0]); i++) {
        tone_async(freqs[i], 1200, 60);
        vTaskDelay(pdMS_TO_TICKS(900));
        pitch_result_t r;
        const uint32_t seq = audio_latest(&r);
        const float c = r.hz > 0 ? 1200.0f * log2f(r.hz / freqs[i]) : 0.0f;
        printf("sweep: %8.2f Hz -> %8.2f Hz  (%+7.2f cents)  clarity %.2f  level %5.1f dB  2nd %.0f Hz  [%lu, %lu us]\n",
               freqs[i], seq ? r.hz : 0.0f, c, r.clarity, r.rms_db, r.second_hz, (unsigned long)seq,
               (unsigned long)r.us);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

/* Raw mic samples, base64'd between '===REC n fs===' and '===END===' (tools/rec.py). */
static void record(int n, float tone_hz)
{
    int16_t *buf = heap_caps_malloc(n * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!buf) {
        printf("rec: no memory\n");
        return;
    }
    if (!audio_listening()) {
        set_listening(true);
        vTaskDelay(pdMS_TO_TICKS(150));
    }
    if (tone_hz > 0) {
        tone_async(tone_hz, n * 1000 / PITCH_FS + 800, 60);
        vTaskDelay(pdMS_TO_TICKS(400));
    }
    if (audio_record(buf, n, n * 1000 / PITCH_FS + 2000) != ESP_OK) {
        printf("rec: failed\n");
        free(buf);
        return;
    }
    printf("\n===REC %d %d===\n", n, PITCH_FS);
    ui_print_base64((const uint8_t *)buf, n * sizeof(int16_t));
    printf("===END===\n");
    fflush(stdout);
    free(buf);
}

static void command(char *line)
{
    char *cmd = strtok(line, " \t");
    if (!cmd) {
        return;
    }
    char *a1 = strtok(NULL, " \t"), *a2 = strtok(NULL, " \t"), *a3 = strtok(NULL, " \t");
    if (!strcmp(cmd, "tone")) {
        const float hz = a1 ? strtof(a1, NULL) : 440.0f;
        const int ms = a2 ? atoi(a2) : 2000;
        const int vol = a3 ? atoi(a3) : 60;
        if (!audio_listening()) {
            set_listening(true);
        }
        tone_async(hz, ms, vol);
    } else if (!strcmp(cmd, "sweep")) {
        sweep();
    } else if (!strcmp(cmd, "listen")) {
        set_listening(true);
    } else if (!strcmp(cmd, "stop")) {
        set_listening(false);
    } else if (!strcmp(cmd, "a4") && a1) {
        set_a4(atoi(a1));
        save_a4(s_a4);
    } else if (!strcmp(cmd, "rec")) {
        record(a1 ? atoi(a1) : PITCH_FS, a2 ? strtof(a2, NULL) : 0.0f);
    } else if (!strcmp(cmd, "bench")) {
        const bool was = audio_listening();
        set_listening(false);
        vTaskDelay(pdMS_TO_TICKS(100));
        pitch_bench();
        set_listening(was);
    } else if (!strcmp(cmd, "snap")) {
        ui_snapshot();
    } else if (!strcmp(cmd, "stat")) {
        static int64_t since;
        display_stats_t d;
        display_stats(&d, &since);
        printf("ui: %.1f fps, LVGL busy %.0f%%, worst %.1f ms, %.1f kpx/frame\n", d.fps, d.busy_pct, d.worst_ms,
               d.kpx_per_frame);
        pitch_result_t r;
        const uint32_t seq = audio_latest(&r);
        printf("heap: internal %u free (%u largest), psram %u free; analysis %lu us (seq %lu)\n",
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
               (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM), (unsigned long)(seq ? r.us : 0),
               (unsigned long)seq);
    } else {
        printf("commands: tone <hz> [ms] [vol] | sweep | listen | stop | a4 <hz> | snap | stat | bench | rec [n] [hz]\n");
    }
}

static void console_task(void *arg)
{
    char line[64];
    int n = 0;
    for (;;) {
        uint8_t c;
        if (uart_read_bytes(UART_NUM_0, &c, 1, portMAX_DELAY) != 1) {
            continue;
        }
        if (c == '\r' || c == '\n') {
            line[n] = 0;
            if (n) {
                command(line);
            }
            n = 0;
        } else if (n < (int)sizeof(line) - 1) {
            line[n++] = (char)c;
        }
    }
}

static void console_start(void)
{
    if (uart_driver_install(UART_NUM_0, 256, 0, 0, NULL, 0) != ESP_OK) {
        ESP_LOGW(TAG, "no console input");
        return;
    }
    uart_vfs_dev_use_driver(UART_NUM_0);
    /* Big enough for 'snap': LVGL renders the whole screen on this task. */
    xTaskCreatePinnedToCore(console_task, "console", 16384, NULL, 2, NULL, 1);
}

/* ---- main --------------------------------------------------------------------------- */

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    s_a4 = load_a4();

    ESP_ERROR_CHECK(board_init());
    if (!display_init()) {
        ESP_LOGE(TAG, "display failed");
    }
    ui_init(s_a4);
    /* Backlight up once the first frame is on the panel. */
    vTaskDelay(pdMS_TO_TICKS(120));
    for (int b = 0; b <= BRIGHTNESS; b += 5) {
        board_set_brightness(b);
        vTaskDelay(pdMS_TO_TICKS(12));
    }
    if (audio_init() != ESP_OK) {
        ui_toast("MIC NOT FOUND");
    }
    console_start();
    ESP_LOGI(TAG, "ready: A4 = %d Hz. Push the wheel to tune.", s_a4);

    /* Input loop. */
    bool down = board_wheel_down();     /* held from power-on: not a press */
    bool ignore_release = down;
    int stable = 0, counts = 0;
    int64_t down_at = esp_timer_get_time(), edge_at = down_at, a4_changed = 0, last_step = 0, power_at = 0;
    bool hint = false, detent = false;
    for (;;) {
        const int64_t now = esp_timer_get_time();

        const bool raw = board_wheel_down();
        if (raw != down) {
            if (++stable >= DEBOUNCE) {
                stable = 0;
                down = raw;
                edge_at = now;
                if (down) {
                    down_at = now;
                } else {
                    const int held = (int)((now - down_at) / 1000);
                    if (hint) {
                        hint = false;
                        ui_toast(NULL);
                    } else if (!ignore_release && held < HOLD_HINT_MS) {
                        set_listening(!audio_listening());
                    }
                    ignore_release = false;
                }
            }
        } else {
            stable = 0;
        }
        if (down && !ignore_release) {
            const int held = (int)((now - down_at) / 1000);
            if (held >= HOLD_OFF_MS) {
                ui_toast("BYE");
                set_listening(false);
                vTaskDelay(pdMS_TO_TICKS(400));
                ESP_LOGI(TAG, "power off");
                board_power_off();
            } else if (held >= HOLD_HINT_MS && !hint) {
                hint = true;
                ui_toast("KEEP HOLDING TO POWER OFF");
            }
        }

        const int c = board_wheel_counts();
        if (raw || down || now - edge_at < PUSH_QUIET_MS * 1000) {
            if (c) {
                ESP_LOGI(TAG, "wheel %+d counts while pushed: ignored", c);
            }
            counts = 0;
        } else if (c) {
            counts += c;
            const int steps = counts / COUNTS_PER_STEP;
            if (steps) {
                counts -= steps * COUNTS_PER_STEP;
                int to = s_a4 + steps;
                if (detent) {
                    to = s_a4;          /* resting at 440: hold until the wheel pauses */
                } else if ((s_a4 < A4_STD) != (to < A4_STD) && s_a4 != A4_STD && to != A4_STD) {
                    to = A4_STD;        /* passing through 440: stop there */
                }
                if (to == A4_STD && s_a4 != A4_STD) {
                    detent = true;
                }
                last_step = now;
                ESP_LOGI(TAG, "wheel %+d counts -> %+d steps, A4 %d -> %d", c, steps, s_a4, to);
                if (to != s_a4) {
                    set_a4(to);
                    a4_changed = now;
                }
                if (!hint) {
                    char msg[40];
                    snprintf(msg, sizeof(msg), "REFERENCE  A4 = %d Hz", s_a4);
                    ui_toast_for(msg, A4_TOAST_MS);
                }
            }
        }
        if (detent && now - last_step > A4_DETENT_MS * 1000) {
            detent = false;
        }
        if (a4_changed && now - a4_changed > A4_SAVE_MS * 1000) {
            a4_changed = 0;
            save_a4(s_a4);
        }

        if (display_take_taps()) {
            set_listening(!audio_listening());
        }

        if (audio_listening() && ui_ms_since_pitch() > AUTO_STOP_MS) {
            ESP_LOGI(TAG, "no pitch for %d s: stopping", AUTO_STOP_MS / 1000);
            set_listening(false);
        }

        if (now - power_at > POWER_POLL_MS * 1000LL || !power_at) {
            power_at = now;
            board_power_t p;
            if (board_read_power(&p) == ESP_OK) {
                ui_set_power(&p);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}
