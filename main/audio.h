#pragma once
/*
 * Microphone capture and pitch analysis, and a test tone on the speaker.
 *
 * The ES7243E (older units: ES7243) mic ADC and the ES8311 speaker DAC share
 * one full-duplex I2S bus at PITCH_FS. While listening, a task on core 1
 * keeps the last PITCH_N samples and analyzes them every AUDIO_HOP new ones.
 */
#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "pitch.h"

#define AUDIO_HOP 1024            /* 21 ms at 48 kHz: ~47 analyses a second */

esp_err_t audio_init(void);

void audio_set_listening(bool on);
bool audio_listening(void);

/* Copies the latest analysis into *out and returns its sequence number,
 * which grows by one per analysis (0: none since listening started). */
uint32_t audio_latest(pitch_result_t *out);

/* Copies the next n mic samples into dst (listening must be on); blocks
 * until done or timeout_ms. */
esp_err_t audio_record(int16_t *dst, int n, int timeout_ms);

/* Plays a sine (with short fades) on the speaker; blocks for ms. */
esp_err_t audio_tone(float hz, int ms, int volume);
