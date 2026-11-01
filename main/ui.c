/*
 * The tuner screen. See ui.h.
 *
 * 412 x 412, round. Top to bottom:
 *   the cents gauge (an arc across the top 140 degrees, ticks every 5 cents,
 *   the in-tune zone, a symmetric indicator and a glowing needle), the cents
 *   readout, the note (letter, sharp, octave), its frequency, the spectrum
 *   (1/8-octave bars, A1 to A7, the fundamental's bar in the gauge's color,
 *   the secondary's in violet, peak caps), the secondary's note, the status.
 *   At the sides: the input level (left), the reference A4 and the battery
 *   (right).
 *
 * Readings are smoothed: a note change needs two agreeing reads, the
 * frequency is the median of the last five, and the needle eases toward it.
 */
#include "ui.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "audio.h"
#include "display.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "note.h"
#include "pitch.h"

LV_FONT_DECLARE(font_note_150);
LV_FONT_DECLARE(font_acc_64);
LV_FONT_DECLARE(font_oct_44);
LV_FONT_DECLARE(font_num_36);
LV_FONT_DECLARE(font_ui_26);
LV_FONT_DECLARE(font_ui_20);
LV_FONT_DECLARE(font_ui_16);

#define C_BG 0x000000
#define C_TRACK 0x161e2a
#define C_ZONE 0x0b4a33
#define C_TICK 0x2d3a4d
#define C_TICK_MAJOR 0x6f8098
#define C_TEXT 0xf1f5f9
#define C_MUTED 0x5f6e84
#define C_DIM 0x2a3546
#define C_GOOD 0x22e39a
#define C_WARN 0xf5c542
#define C_BAD 0xff4d6a
#define C_CYAN 0x2ee6d6
#define C_CYAN_DARK 0x0a4f5a
#define C_VIOLET 0xb46cff

#define RES 412
#define CX (RES / 2)
#define CY (RES / 2)
#define ARC_SIZE 372            /* gauge outer diameter */
#define ARC_W 10
#define ARC_ROT 200             /* gauge start, degrees clockwise from 3 o'clock */
#define ARC_SPAN 140            /* ... to 340: centered on 12 o'clock */
#define NEEDLE_R ((ARC_SIZE - ARC_W) / 2)
#define NEEDLE_D 22
#define ZONE_CENTS 5.0f         /* the green zone, +-cents */
#define GOOD_CENTS 3.0f         /* in tune */

#define NOTE_TOP 117            /* the note letter's box: 105 px tall */
#define NOTE_H 105
#define SPEC_X 62
#define SPEC_Y 276
#define SPEC_H 54
#define BAR_PITCH 6
#define BAR_W 4
#define SPEC_W (PITCH_BARS * BAR_PITCH)
#define DB_FLOOR -82.0f         /* bars: empty here ... */
#define DB_TOP -18.0f           /* ... full here */
#define BAR_FALL 0.035f         /* per frame */
#define CAP_HOLD 15             /* frames */
#define CAP_FALL 0.012f

#define FRAME_MS 33
#define TEXT_MS 90              /* numbers change at most this often */
#define CLARITY_SHOW 0.80f
#define LEVEL_SHOW -60.0f
#define LIVE_US 1200000         /* a reading this old dims */
#define BLANK_US 8000000        /* ... and this old clears */
#define HIST 5
#define SECOND_RANGE_DB 25.0f   /* a secondary this far under the strongest peak isn't shown */

static const char *TAG = "ui";

static struct {
    lv_obj_t *ring, *halo, *halo2, *track, *zone, *meter, *scale, *needle;
    lv_obj_t *cents, *note, *acc, *oct, *hz;
    lv_obj_t *spec, *second;
    lv_obj_t *status_row, *status_dot, *status;
    lv_obj_t *a4_cap, *a4, *batt;
    lv_obj_t *level, *level_cap;
    lv_obj_t *toast, *toast_lbl;
} w;

static struct {
    bool listening;
    int a4;
    uint32_t seq;
    int midi;                   /* the note shown, -1 for none */
    int cand_midi, cand_n;      /* a different note, read cand_n times in a row */
    float hist[HIST];
    int hist_n, hist_i;
    float cents_target, cents_shown, hz_shown;
    int64_t last_pitch_us, listen_start_us, last_text_us, a4_flash_us, toast_until_us;
    float bar[PITCH_BARS], cap[PITCH_BARS];
    int cap_hold[PITCH_BARS];
    int bar_primary, bar_second;
    float second_hz;
    int second_harmonic;
    /* What's on screen, to skip redundant updates. */
    uint32_t col_state;
    char cents_txt[16], hz_txt[24], second_txt[48];
    int needle_x, needle_y, arc_val;
    bool dim;
} s = { .a4 = 440, .midi = -1, .cand_midi = -1, .bar_primary = -1, .bar_second = -1, .col_state = 1 };

/* ---- helpers ----------------------------------------------------------------- */

/* The reference at rest: amber when it isn't the standard 440, so a moved
 * reference (which renames every note) can't go unnoticed. */
static uint32_t a4_color(int a4)
{
    return a4 == 440 ? C_TEXT : C_WARN;
}

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, text);
    return l;
}

static lv_obj_t *plain(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static lv_obj_t *arc(lv_obj_t *parent, int start, int end, uint32_t main_color, bool indicator)
{
    lv_obj_t *a = lv_arc_create(parent);
    lv_obj_remove_style_all(a);
    lv_obj_remove_flag(a, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(a, ARC_SIZE, ARC_SIZE);
    lv_obj_center(a);
    lv_arc_set_rotation(a, ARC_ROT);
    lv_arc_set_bg_angles(a, start, end);
    lv_obj_set_style_arc_width(a, ARC_W, LV_PART_MAIN);
    lv_obj_set_style_arc_color(a, lv_color_hex(main_color), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(a, indicator ? LV_OPA_TRANSP : LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(a, !indicator && start == 0, LV_PART_MAIN);
    lv_obj_set_style_arc_width(a, ARC_W, LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(a, indicator ? LV_OPA_COVER : LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(a, true, LV_PART_INDICATOR);
    return a;
}

/* Gauge color for a deviation: green in tune, amber close, red far. */
static lv_color_t tune_color(float cents)
{
    const float c = fabsf(cents);
    if (c <= GOOD_CENTS) {
        return lv_color_hex(C_GOOD);
    }
    if (c <= 12.0f) {
        return lv_color_mix(lv_color_hex(C_WARN), lv_color_hex(C_GOOD), (uint8_t)(255 * (c - GOOD_CENTS) / 9.0f));
    }
    if (c <= 30.0f) {
        return lv_color_mix(lv_color_hex(C_BAD), lv_color_hex(C_WARN), (uint8_t)(255 * (c - 12.0f) / 18.0f));
    }
    return lv_color_hex(C_BAD);
}

static int bar_of(float hz)
{
    if (hz <= 0.0f) {
        return -1;
    }
    const int i = (int)lroundf(PITCH_BARS_PER_OCT * log2f(hz / PITCH_BAR_LO_HZ));
    return i >= 0 && i < PITCH_BARS ? i : -1;
}

static void set_text(lv_obj_t *l, char *shown, size_t size, const char *text)
{
    if (strcmp(shown, text)) {
        const size_t n = strnlen(text, size - 1);
        memcpy(shown, text, n);
        shown[n] = 0;
        lv_label_set_text(l, text);
    }
}

/* ---- the note ---------------------------------------------------------------- */

static void layout_note(char letter, bool sharp, int octave)
{
    char t[4] = { letter, 0 };
    lv_label_set_text(w.note, t);
    const int lw = lv_font_get_glyph_width(&font_note_150, (uint32_t)letter, 0);
    int rw = 0;
    if (octave >= 0) {
        char o[12];
        snprintf(o, sizeof(o), "%d", octave);
        lv_label_set_text(w.oct, o);
        lv_point_t sz;
        lv_text_get_size(&sz, o, &font_oct_44, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        rw = sz.x;
        lv_obj_remove_flag(w.oct, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(w.oct, LV_OBJ_FLAG_HIDDEN);
    }
    if (sharp) {
        const int aw = lv_font_get_glyph_width(&font_acc_64, 0x266F, 0);
        rw = aw > rw ? aw : rw;
        lv_obj_remove_flag(w.acc, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(w.acc, LV_OBJ_FLAG_HIDDEN);
    }
    const int gap = rw ? 6 : 0;
    const int x = CX - (lw + gap + rw) / 2;
    lv_obj_set_pos(w.note, x, NOTE_TOP);
    lv_obj_set_pos(w.acc, x + lw + gap - 2, NOTE_TOP - 4);
    lv_obj_set_pos(w.oct, x + lw + gap, NOTE_TOP + NOTE_H - 31);
}

static void show_note(int midi)
{
    if (midi < 0) {
        layout_note('-', false, -1);
        return;
    }
    const int pc = ((midi % 12) + 12) % 12;
    layout_note(note_letter(pc), note_sharp(pc), midi / 12 - 1);
}

static void note_name(char *out, size_t size, float hz, float a4)
{
    const note_t n = note_from_hz(hz, a4);
    snprintf(out, size, "%c%s%d", note_letter(n.pc), note_sharp(n.pc) ? NOTE_SHARP_UTF8 : "", n.octave);
}

/* ---- the spectrum -------------------------------------------------------------- */

static void spec_draw(lv_event_t *e)
{
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(lv_event_get_target(e), &a);
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.radius = 1;
    d.bg_grad.dir = LV_GRAD_DIR_VER;
    d.bg_grad.stops_count = 2;
    d.bg_grad.stops[0].frac = 0;
    d.bg_grad.stops[0].opa = LV_OPA_COVER;
    d.bg_grad.stops[1].frac = 255;
    d.bg_grad.stops[1].opa = LV_OPA_COVER;
    lv_draw_rect_dsc_t cap;
    lv_draw_rect_dsc_init(&cap);
    cap.bg_color = lv_color_hex(0x8fa3bb);
    cap.bg_opa = LV_OPA_70;

    const lv_color_t primary = s.dim ? lv_color_hex(C_MUTED) : tune_color(s.cents_shown);
    for (int i = 0; i < PITCH_BARS; i++) {
        int h = (int)(s.bar[i] * SPEC_H);
        if (h < 2) {
            h = 2;
        }
        lv_color_t top = lv_color_hex(C_CYAN), bottom = lv_color_hex(C_CYAN_DARK);
        if (i == s.bar_primary) {
            top = primary;
            bottom = lv_color_mix(primary, lv_color_hex(C_BG), 110);
        } else if (i == s.bar_second) {
            top = lv_color_hex(C_VIOLET);
            bottom = lv_color_mix(lv_color_hex(C_VIOLET), lv_color_hex(C_BG), 110);
        } else if (h <= 2) {
            top = bottom = lv_color_hex(C_DIM);
        }
        d.bg_color = top;
        d.bg_grad.stops[0].color = top;
        d.bg_grad.stops[1].color = bottom;
        const lv_area_t r = {
            .x1 = a.x1 + i * BAR_PITCH,
            .x2 = a.x1 + i * BAR_PITCH + BAR_W - 1,
            .y1 = a.y2 - h + 1,
            .y2 = a.y2,
        };
        lv_draw_rect(layer, &d, &r);
        const int ch = (int)(s.cap[i] * SPEC_H);
        if (ch > h + 2) {
            const lv_area_t c = { .x1 = r.x1, .x2 = r.x2, .y1 = a.y2 - ch, .y2 = a.y2 - ch + 1 };
            lv_draw_rect(layer, &cap, &c);
        }
    }
}

static void spec_update(const pitch_result_t *r)
{
    for (int i = 0; i < PITCH_BARS; i++) {
        if (r) {
            float v = (r->bars_db[i] - DB_FLOOR) / (DB_TOP - DB_FLOOR);
            v = v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
            if (v > s.bar[i]) {
                s.bar[i] = v;
            }
        }
        if (s.bar[i] >= s.cap[i]) {
            s.cap[i] = s.bar[i];
            s.cap_hold[i] = CAP_HOLD;
        }
    }
}

static bool spec_decay(void)
{
    bool any = false;
    for (int i = 0; i < PITCH_BARS; i++) {
        if (s.bar[i] > 0.0f) {
            s.bar[i] = fmaxf(0.0f, s.bar[i] - BAR_FALL);
            any = true;
        }
        if (s.cap_hold[i] > 0) {
            s.cap_hold[i]--;
            any = true;
        } else if (s.cap[i] > 0.0f) {
            s.cap[i] = fmaxf(0.0f, s.cap[i] - CAP_FALL);
            any = true;
        }
    }
    return any;
}

/* ---- per-frame update ----------------------------------------------------------- */

static float median(void)
{
    float v[HIST];
    const int n = s.hist_n;
    for (int i = 0; i < n; i++) {
        v[i] = s.hist[i];
    }
    for (int i = 1; i < n; i++) {
        for (int j = i; j > 0 && v[j] < v[j - 1]; j--) {
            const float t = v[j];
            v[j] = v[j - 1];
            v[j - 1] = t;
        }
    }
    return n & 1 ? v[n / 2] : 0.5f * (v[n / 2 - 1] + v[n / 2]);
}

static void on_pitch(float hz, int64_t now)
{
    const note_t n = note_from_hz(hz, (float)s.a4);
    if (n.midi != s.midi) {
        s.cand_n = n.midi == s.cand_midi ? s.cand_n + 1 : 1;
        s.cand_midi = n.midi;
        const bool stale = now - s.last_pitch_us > 300000;
        if (s.midi >= 0 && !stale && s.cand_n < 2) {
            return;
        }
        s.midi = n.midi;
        s.hist_n = 0;
        s.hist_i = 0;
        show_note(s.midi);
    }
    s.cand_n = 0;
    s.hist[s.hist_i] = hz;
    s.hist_i = (s.hist_i + 1) % HIST;
    if (s.hist_n < HIST) {
        s.hist_n++;
    }
    s.hz_shown = median();
    float c = 1200.0f * log2f(s.hz_shown / note_hz(s.midi, (float)s.a4));
    s.cents_target = c < -50.0f ? -50.0f : c > 50.0f ? 50.0f : c;
    s.last_pitch_us = now;
}

static void apply_colors(lv_color_t c, bool dim, bool in_tune)
{
    const uint32_t key = lv_color_to_u32(c) ^ (dim ? 0x1000000u : 0) ^ (in_tune ? 0x2000000u : 0);
    if (key == s.col_state) {
        return;
    }
    s.col_state = key;
    const lv_color_t shown = dim ? lv_color_hex(C_MUTED) : c;
    lv_obj_set_style_arc_color(w.meter, shown, LV_PART_INDICATOR);
    lv_obj_set_style_border_color(w.needle, shown, 0);
    lv_obj_set_style_shadow_color(w.needle, shown, 0);
    lv_obj_set_style_shadow_opa(w.needle, dim ? LV_OPA_TRANSP : LV_OPA_70, 0);
    lv_obj_set_style_bg_color(w.needle, dim ? lv_color_hex(C_MUTED) : lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_text_color(w.cents, shown, 0);
    const lv_color_t note = dim ? lv_color_hex(C_DIM) : in_tune ? lv_color_hex(C_GOOD) : lv_color_hex(C_TEXT);
    lv_obj_set_style_text_color(w.note, note, 0);
    lv_obj_set_style_text_color(w.acc, note, 0);
    lv_obj_set_style_text_color(w.oct, dim ? lv_color_hex(C_DIM) : lv_color_hex(C_MUTED), 0);
    lv_obj_set_style_text_color(w.hz, dim ? lv_color_hex(C_DIM) : lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_text_color(w.second, dim ? lv_color_hex(0x4a3a66) : lv_color_hex(C_VIOLET), 0);
    lv_obj_set_style_arc_opa(w.halo, in_tune ? LV_OPA_COVER : LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(w.halo2, in_tune ? LV_OPA_40 : LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_arc_color(w.zone, in_tune ? lv_color_hex(0x16a36c) : lv_color_hex(C_ZONE), LV_PART_MAIN);
}

static void frame(lv_timer_t *t)
{
    const int64_t now = esp_timer_get_time();
    pitch_result_t r;
    const uint32_t seq = s.listening ? audio_latest(&r) : 0;
    bool spec_dirty = false;
    if (seq && seq != s.seq) {
        s.seq = seq;
        spec_update(&r);
        spec_dirty = true;
        const int lvl = (int)((r.rms_db + 72.0f) * 100.0f / 60.0f);
        lv_bar_set_value(w.level, lvl < 0 ? 0 : lvl > 100 ? 100 : lvl, LV_ANIM_OFF);
        const bool valid = r.hz > 0.0f && r.clarity >= CLARITY_SHOW && r.rms_db >= LEVEL_SHOW;
        if (valid) {
            on_pitch(r.hz, now);
            /* Only a secondary that's a real part of the sound, not room noise. */
            const bool loud = r.second_db > DB_FLOOR + 10.0f && r.n_peaks && r.second_db > r.peaks[0].db - SECOND_RANGE_DB;
            s.second_hz = loud ? r.second_hz : 0.0f;
            s.second_harmonic = r.second_harmonic;
        }
    } else if (!s.listening) {
        lv_bar_set_value(w.level, 0, LV_ANIM_OFF);
    }

    if (s.listening && s.midi >= 0 && now - s.last_pitch_us > BLANK_US) {
        s.midi = -1;
        s.second_hz = 0.0f;
        show_note(-1);
        set_text(w.cents, s.cents_txt, sizeof(s.cents_txt), " ");
        set_text(w.hz, s.hz_txt, sizeof(s.hz_txt), " ");
        set_text(w.second, s.second_txt, sizeof(s.second_txt), " ");
    }
    const bool have = s.midi >= 0;    /* after the blanking: the texts stay cleared */
    const bool live = s.listening && have && now - s.last_pitch_us < LIVE_US;
    s.dim = !live;

    /* The needle eases toward the reading. A stale reading holds it (dimmed);
     * with no note at all it glides back to the center. */
    if (live) {
        s.cents_shown += (s.cents_target - s.cents_shown) * 0.35f;
    } else if (s.midi < 0 && s.cents_shown != 0.0f) {
        s.cents_shown *= 0.85f;
        if (fabsf(s.cents_shown) < 0.05f) {
            s.cents_shown = 0.0f;
        }
    }
    const bool in_tune = live && fabsf(s.cents_shown) <= GOOD_CENTS;
    apply_colors(tune_color(s.cents_shown), !live, in_tune);
    const int val = (int)lroundf(s.cents_shown * 10.0f);
    if (val != s.arc_val) {
        s.arc_val = val;
        lv_arc_set_value(w.meter, val);
    }
    const float ang = (ARC_ROT + ARC_SPAN * 0.5f + s.cents_shown / 50.0f * ARC_SPAN * 0.5f) * (float)M_PI / 180.0f;
    const int nx = (int)lroundf(CX + NEEDLE_R * cosf(ang)) - NEEDLE_D / 2;
    const int ny = (int)lroundf(CY + NEEDLE_R * sinf(ang)) - NEEDLE_D / 2;
    if (nx != s.needle_x || ny != s.needle_y) {
        s.needle_x = nx;
        s.needle_y = ny;
        lv_obj_set_pos(w.needle, nx, ny);
    }

    if (have && now - s.last_text_us >= TEXT_MS * 1000) {
        s.last_text_us = now;
        char b[48];
        const float c = roundf(s.cents_shown * 10.0f) / 10.0f;
        snprintf(b, sizeof(b), "%+.1f" CENT_UTF8, c == 0.0f ? 0.0f : c);   /* no "-0.0" */
        set_text(w.cents, s.cents_txt, sizeof(s.cents_txt), b);
        snprintf(b, sizeof(b), "%.1f Hz", s.hz_shown);
        set_text(w.hz, s.hz_txt, sizeof(s.hz_txt), b);
        if (s.second_hz > 0.0f) {
            char name[16];
            note_name(name, sizeof(name), s.second_hz, (float)s.a4);
            if (s.second_harmonic) {
                snprintf(b, sizeof(b), "%s  %.0f Hz  " MIDDOT_UTF8 "  H%d", name, s.second_hz, s.second_harmonic);
            } else {
                snprintf(b, sizeof(b), "%s  %.0f Hz", name, s.second_hz);
            }
        } else {
            b[0] = ' ';
            b[1] = 0;
        }
        set_text(w.second, s.second_txt, sizeof(s.second_txt), b);
    }

    const int bp = have ? bar_of(s.hz_shown) : -1;
    const int bs = have ? bar_of(s.second_hz) : -1;
    if (bp != s.bar_primary || bs != s.bar_second) {
        s.bar_primary = bp;
        s.bar_second = bs;
        spec_dirty = true;
    }
    if (spec_decay() || spec_dirty) {
        lv_obj_invalidate(w.spec);
    }

    if (s.a4_flash_us && now - s.a4_flash_us > 1500000) {
        s.a4_flash_us = 0;
        lv_obj_set_style_text_color(w.a4, lv_color_hex(a4_color(s.a4)), 0);
    }
    if (s.toast_until_us && now > s.toast_until_us) {
        s.toast_until_us = 0;
        lv_obj_add_flag(w.toast, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ---- building the screen --------------------------------------------------------- */

static void pulse(void *obj, int32_t v)
{
    lv_obj_set_style_bg_opa(obj, (lv_opa_t)v, 0);
}

void ui_init(int a4)
{
    s.a4 = a4;
    display_lock(-1);
    lv_obj_t *scr = lv_screen_active();
    lv_obj_remove_style_all(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* Rim, and a halo over the gauge's center that lights up in tune. The
     * halo is two short arcs rather than a shadow: LVGL recomputes a shadow
     * on every redraw, and one around the whole rim cost ~600 ms a frame. */
    w.ring = plain(scr);
    lv_obj_set_size(w.ring, RES - 4, RES - 4);
    lv_obj_center(w.ring);
    lv_obj_set_style_radius(w.ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(w.ring, 2, 0);
    lv_obj_set_style_border_color(w.ring, lv_color_hex(C_TRACK), 0);
    w.halo = lv_arc_create(scr);
    lv_obj_remove_style_all(w.halo);
    lv_obj_remove_flag(w.halo, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(w.halo, RES - 8, RES - 8);
    lv_obj_center(w.halo);
    lv_arc_set_bg_angles(w.halo, 250, 290);
    lv_obj_set_style_arc_width(w.halo, 4, LV_PART_MAIN);
    lv_obj_set_style_arc_color(w.halo, lv_color_hex(C_GOOD), LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(w.halo, true, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(w.halo, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(w.halo, LV_OPA_TRANSP, LV_PART_INDICATOR);
    w.halo2 = lv_arc_create(scr);
    lv_obj_remove_style_all(w.halo2);
    lv_obj_remove_flag(w.halo2, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(w.halo2, RES - 20, RES - 20);
    lv_obj_center(w.halo2);
    lv_arc_set_bg_angles(w.halo2, 240, 300);
    lv_obj_set_style_arc_width(w.halo2, 6, LV_PART_MAIN);
    lv_obj_set_style_arc_color(w.halo2, lv_color_hex(C_GOOD), LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(w.halo2, true, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(w.halo2, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(w.halo2, LV_OPA_TRANSP, LV_PART_INDICATOR);

    /* Gauge: track, in-tune zone, indicator, ticks, needle. */
    w.track = arc(scr, 0, ARC_SPAN, C_TRACK, false);
    const int zone = (int)lroundf(ZONE_CENTS / 50.0f * ARC_SPAN / 2);
    w.zone = arc(scr, ARC_SPAN / 2 - zone, ARC_SPAN / 2 + zone, C_ZONE, false);
    w.meter = arc(scr, 0, ARC_SPAN, C_TRACK, true);
    lv_arc_set_mode(w.meter, LV_ARC_MODE_SYMMETRICAL);
    lv_arc_set_range(w.meter, -500, 500);
    lv_arc_set_value(w.meter, 0);
    lv_obj_set_style_arc_color(w.meter, lv_color_hex(C_GOOD), LV_PART_INDICATOR);

    w.scale = lv_scale_create(scr);
    lv_obj_remove_style_all(w.scale);
    lv_obj_set_size(w.scale, ARC_SIZE - 2 * ARC_W - 12, ARC_SIZE - 2 * ARC_W - 12);
    lv_obj_center(w.scale);
    lv_scale_set_mode(w.scale, LV_SCALE_MODE_ROUND_INNER);
    lv_scale_set_range(w.scale, -50, 50);
    lv_scale_set_total_tick_count(w.scale, 21);
    lv_scale_set_major_tick_every(w.scale, 5);
    lv_scale_set_label_show(w.scale, true);
    lv_scale_set_angle_range(w.scale, ARC_SPAN);
    lv_scale_set_rotation(w.scale, ARC_ROT);
    static const char *ticks[] = { "-50", "-25", "0", "+25", "+50", NULL };
    lv_scale_set_text_src(w.scale, ticks);
    lv_obj_set_style_arc_width(w.scale, 0, LV_PART_MAIN);
    lv_obj_set_style_length(w.scale, 10, LV_PART_INDICATOR);
    lv_obj_set_style_line_width(w.scale, 2, LV_PART_INDICATOR);
    lv_obj_set_style_line_color(w.scale, lv_color_hex(C_TICK_MAJOR), LV_PART_INDICATOR);
    lv_obj_set_style_text_font(w.scale, &font_ui_16, LV_PART_INDICATOR);
    lv_obj_set_style_text_color(w.scale, lv_color_hex(C_MUTED), LV_PART_INDICATOR);
    lv_obj_set_style_length(w.scale, 5, LV_PART_ITEMS);
    lv_obj_set_style_line_width(w.scale, 2, LV_PART_ITEMS);
    lv_obj_set_style_line_color(w.scale, lv_color_hex(C_TICK), LV_PART_ITEMS);

    w.needle = plain(scr);
    lv_obj_set_size(w.needle, NEEDLE_D, NEEDLE_D);
    lv_obj_set_style_radius(w.needle, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(w.needle, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(w.needle, lv_color_hex(C_MUTED), 0);
    lv_obj_set_style_border_width(w.needle, 4, 0);
    lv_obj_set_style_border_color(w.needle, lv_color_hex(C_MUTED), 0);
    lv_obj_set_style_shadow_width(w.needle, 26, 0);
    lv_obj_set_style_shadow_spread(w.needle, 2, 0);
    lv_obj_set_style_shadow_opa(w.needle, LV_OPA_TRANSP, 0);
    s.needle_x = s.needle_y = -1000;
    s.arc_val = 1;

    /* Readouts. */
    w.cents = label(scr, &font_ui_26, C_MUTED, " ");
    lv_obj_align(w.cents, LV_ALIGN_TOP_MID, 0, 80);
    w.note = label(scr, &font_note_150, C_DIM, "-");
    w.acc = label(scr, &font_acc_64, C_DIM, NOTE_SHARP_UTF8);
    w.oct = label(scr, &font_oct_44, C_DIM, "4");
    show_note(-1);
    w.hz = label(scr, &font_num_36, C_DIM, " ");
    lv_obj_align(w.hz, LV_ALIGN_TOP_MID, 0, 234);

    /* Spectrum, with A2..A6 marked under it. */
    w.spec = plain(scr);
    lv_obj_set_pos(w.spec, SPEC_X, SPEC_Y);
    lv_obj_set_size(w.spec, SPEC_W, SPEC_H);
    lv_obj_add_event_cb(w.spec, spec_draw, LV_EVENT_DRAW_MAIN, NULL);
    for (int o = 2; o <= 6; o++) {
        char t[4];
        snprintf(t, sizeof(t), "A%d", o);
        lv_obj_t *l = label(scr, &font_ui_16, C_MUTED, t);
        const int bar = (o - 1) * PITCH_BARS_PER_OCT;
        lv_obj_align(l, LV_ALIGN_TOP_LEFT, SPEC_X + bar * BAR_PITCH + BAR_W / 2 - 9, SPEC_Y + SPEC_H + 3);
    }
    w.second = label(scr, &font_ui_20, C_VIOLET, " ");
    lv_obj_align(w.second, LV_ALIGN_TOP_MID, 0, 353);

    /* Status. */
    w.status_row = plain(scr);
    lv_obj_set_size(w.status_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(w.status_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(w.status_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(w.status_row, 8, 0);
    lv_obj_align(w.status_row, LV_ALIGN_TOP_MID, 0, 377);
    w.status_dot = plain(w.status_row);
    lv_obj_set_size(w.status_dot, 10, 10);
    lv_obj_set_style_radius(w.status_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(w.status_dot, lv_color_hex(C_BAD), 0);
    lv_obj_set_style_bg_opa(w.status_dot, LV_OPA_COVER, 0);
    lv_obj_add_flag(w.status_dot, LV_OBJ_FLAG_HIDDEN);
    w.status = label(w.status_row, &font_ui_20, C_MUTED, "PUSH TO TUNE");

    /* Sides: input level (left), reference and battery (right). */
    w.level = lv_bar_create(scr);
    lv_obj_remove_style_all(w.level);
    lv_obj_set_size(w.level, 6, 84);
    lv_obj_set_pos(w.level, 42, 164);
    lv_bar_set_range(w.level, 0, 100);
    lv_obj_set_style_bg_color(w.level, lv_color_hex(C_TRACK), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(w.level, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(w.level, 3, LV_PART_MAIN);
    lv_obj_set_style_bg_color(w.level, lv_color_hex(C_GOOD), LV_PART_INDICATOR);
    lv_obj_set_style_bg_grad_color(w.level, lv_color_hex(C_CYAN_DARK), LV_PART_INDICATOR);
    lv_obj_set_style_bg_grad_dir(w.level, LV_GRAD_DIR_VER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(w.level, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(w.level, 3, LV_PART_INDICATOR);
    w.level_cap = label(scr, &font_ui_16, C_MUTED, "IN");
    lv_obj_align_to(w.level_cap, w.level, LV_ALIGN_OUT_BOTTOM_MID, 0, 6);

    w.a4_cap = label(scr, &font_ui_16, C_MUTED, "A4");
    lv_obj_align(w.a4_cap, LV_ALIGN_TOP_MID, 160, 184);
    w.a4 = label(scr, &font_ui_26, C_TEXT, "440");
    lv_obj_align(w.a4, LV_ALIGN_TOP_MID, 160, 202);
    w.batt = label(scr, &lv_font_montserrat_14, C_MUTED, " ");
    lv_obj_align(w.batt, LV_ALIGN_TOP_MID, 156, 240);
    ui_set_a4(a4);
    s.a4_flash_us = 0;
    lv_obj_set_style_text_color(w.a4, lv_color_hex(a4_color(a4)), 0);

    /* Toast. */
    w.toast = plain(scr);
    lv_obj_set_size(w.toast, 300, 76);
    lv_obj_center(w.toast);
    lv_obj_set_style_radius(w.toast, 38, 0);
    lv_obj_set_style_bg_color(w.toast, lv_color_hex(0x0b1220), 0);
    lv_obj_set_style_bg_opa(w.toast, LV_OPA_90, 0);
    lv_obj_set_style_border_width(w.toast, 2, 0);
    lv_obj_set_style_border_color(w.toast, lv_color_hex(C_CYAN), 0);
    w.toast_lbl = label(w.toast, &font_ui_20, C_TEXT, "");
    lv_obj_center(w.toast_lbl);
    lv_obj_add_flag(w.toast, LV_OBJ_FLAG_HIDDEN);

    lv_timer_create(frame, FRAME_MS, NULL);
    display_unlock();
}

/* ---- state from outside ------------------------------------------------------------ */

void ui_set_listening(bool on)
{
    display_lock(-1);
    s.listening = on;
    if (on) {
        s.listen_start_us = esp_timer_get_time();
        s.seq = 0;
        lv_obj_remove_flag(w.status_dot, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(w.status, "LISTENING");
        lv_obj_set_style_text_color(w.status, lv_color_hex(C_TEXT), 0);
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, w.status_dot);
        lv_anim_set_exec_cb(&a, pulse);
        lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_20);
        lv_anim_set_duration(&a, 700);
        lv_anim_set_playback_duration(&a, 700);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_start(&a);
    } else {
        lv_anim_delete(w.status_dot, pulse);
        lv_obj_add_flag(w.status_dot, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(w.status, "PUSH TO TUNE");
        lv_obj_set_style_text_color(w.status, lv_color_hex(C_MUTED), 0);
    }
    display_unlock();
}

void ui_set_a4(int a4)
{
    display_lock(-1);
    s.a4 = a4;
    lv_label_set_text_fmt(w.a4, "%d", a4);
    lv_obj_set_style_text_color(w.a4_cap, lv_color_hex(a4 == 440 ? C_MUTED : C_WARN), 0);
    lv_obj_set_style_text_color(w.a4, lv_color_hex(C_CYAN), 0);
    s.a4_flash_us = esp_timer_get_time();
    if (s.midi >= 0 && s.hist_n) {
        /* The same sound against the new reference. */
        const note_t n = note_from_hz(s.hz_shown, (float)a4);
        s.midi = n.midi;
        show_note(s.midi);
        s.cents_target = n.cents;
    }
    display_unlock();
}

void ui_set_power(const board_power_t *p)
{
    char t[24];
    if (p->charging) {
        snprintf(t, sizeof(t), LV_SYMBOL_CHARGE " %d%%", p->pct);
    } else if (p->pct < 0) {
        snprintf(t, sizeof(t), "%s", p->usb ? LV_SYMBOL_USB : " ");
    } else {
        const char *icon = p->pct > 80 ? LV_SYMBOL_BATTERY_FULL
                           : p->pct > 55 ? LV_SYMBOL_BATTERY_3
                           : p->pct > 30 ? LV_SYMBOL_BATTERY_2
                           : p->pct > 10 ? LV_SYMBOL_BATTERY_1
                                         : LV_SYMBOL_BATTERY_EMPTY;
        snprintf(t, sizeof(t), "%s %d%%", icon, p->pct);
    }
    display_lock(-1);
    lv_label_set_text(w.batt, t);
    lv_obj_set_style_text_color(w.batt, lv_color_hex(p->pct >= 0 && p->pct <= 10 && !p->usb ? C_BAD : C_MUTED), 0);
    display_unlock();
}

void ui_toast_for(const char *msg, int ms)
{
    display_lock(-1);
    if (msg) {
        lv_label_set_text(w.toast_lbl, msg);
        lv_obj_remove_flag(w.toast, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(w.toast, LV_OBJ_FLAG_HIDDEN);
    }
    s.toast_until_us = msg && ms > 0 ? esp_timer_get_time() + (int64_t)ms * 1000 : 0;
    display_unlock();
}

void ui_toast(const char *msg)
{
    ui_toast_for(msg, 0);
}

uint32_t ui_ms_since_pitch(void)
{
    display_lock(-1);
    const int64_t since = !s.listening                       ? esp_timer_get_time()
                          : s.last_pitch_us > s.listen_start_us ? s.last_pitch_us
                                                                : s.listen_start_us;
    display_unlock();
    return (uint32_t)((esp_timer_get_time() - since) / 1000);
}

/* ---- screenshot --------------------------------------------------------------------- */

void ui_print_base64(const uint8_t *in, size_t n)
{
    static const char tab[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char line[80];
    int o = 0;
    for (size_t i = 0; i < n; i += 3) {
        const uint32_t v = in[i] << 16 | (i + 1 < n ? in[i + 1] << 8 : 0) | (i + 2 < n ? in[i + 2] : 0);
        line[o++] = tab[v >> 18 & 63];
        line[o++] = tab[v >> 12 & 63];
        line[o++] = i + 1 < n ? tab[v >> 6 & 63] : '=';
        line[o++] = i + 2 < n ? tab[v & 63] : '=';
        if (o >= 76) {
            line[o] = 0;
            printf("%s\n", line);
            o = 0;
        }
    }
    if (o) {
        line[o] = 0;
        printf("%s\n", line);
    }
}

void ui_snapshot(void)
{
    display_lock(-1);
    lv_draw_buf_t *buf = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_RGB565);
    display_unlock();
    if (!buf) {
        ESP_LOGE(TAG, "snapshot: no memory");
        return;
    }
    /* Runs of equal pixels as (count, pixel) pairs of little-endian uint16. */
    const int wpx = buf->header.w, hpx = buf->header.h;
    printf("\n===SNAP %d %d===\n", wpx, hpx);
    static uint8_t chunk[600];
    size_t n = 0;
    for (int y = 0; y < hpx; y++) {
        const uint16_t *row = (const uint16_t *)(buf->data + y * buf->header.stride);
        for (int x = 0; x < wpx;) {
            const uint16_t v = row[x];
            int run = 1;
            while (x + run < wpx && row[x + run] == v && run < 65535) {
                run++;
            }
            x += run;
            chunk[n++] = run & 0xff;
            chunk[n++] = run >> 8;
            chunk[n++] = v & 0xff;
            chunk[n++] = v >> 8;
            if (n == sizeof(chunk)) {
                ui_print_base64(chunk, n);
                n = 0;
            }
        }
    }
    ui_print_base64(chunk, n);
    printf("===END===\n");
    fflush(stdout);
    lv_draw_buf_destroy(buf);
}
