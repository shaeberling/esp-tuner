#pragma once
/*
 * LVGL on the Watcher's 412x412 round SPD2010 panel (QSPI) with its touch
 * controller. LVGL runs on its own task on core 0; take the lock around
 * any LVGL call made from another task.
 */
#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

/* Panel, LVGL, touch, and the LVGL task. Call after board_init(). */
bool display_init(void);

bool display_lock(int timeout_ms);
void display_unlock(void);

/* How many times a finger tapped the screen since the last call
 * (pressed and released without moving much). */
int display_take_taps(void);

typedef struct {
    float fps;            /* frames rendered a second */
    float busy_pct;       /* of the LVGL task's core */
    float worst_ms;       /* longest lv_timer_handler() */
    float kpx_per_frame;  /* pixels sent to the panel per frame, thousands */
} display_stats_t;
/* Since the last call (*since: its time, updated). */
void display_stats(display_stats_t *out, int64_t *since);
