#pragma once
/*
 * The tuner screen: a cents gauge across the top of the round display, the
 * note in the middle, its frequency, a spectrum of the last block with the
 * fundamental and the secondary peak marked, the secondary's note, and the
 * status. A timer in the LVGL task pulls analyses from audio.c.
 * All functions take the display lock.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "board.h"

void ui_init(int a4);
void ui_set_listening(bool on);
void ui_set_a4(int a4);
void ui_set_power(const board_power_t *p);
/* A message over everything; NULL hides it. */
void ui_toast(const char *msg);
/* The same, hiding itself after ms. */
void ui_toast_for(const char *msg, int ms);
/* Milliseconds since the last clear pitch (or since listening started). */
uint32_t ui_ms_since_pitch(void);
/* Prints the screen, run-length encoded and base64'd, to stdout
 * (tools/snap.py turns it into a PNG). */
void ui_snapshot(void);
/* Prints bytes as base64, 76 characters a line. */
void ui_print_base64(const uint8_t *in, size_t n);
