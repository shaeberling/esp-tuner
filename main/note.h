#pragma once
/* Frequency to equal-tempered note, relative to a reference A4. */
#include <math.h>

typedef struct {
    int midi;          /* 69 = A4 */
    int octave;        /* scientific pitch notation: middle C is C4 */
    int pc;            /* pitch class, 0 = C .. 11 = B */
    float cents;       /* -50 .. +50 from the note */
} note_t;

static inline float note_hz(int midi, float a4)
{
    return a4 * exp2f((midi - 69) / 12.0f);
}

static inline note_t note_from_hz(float hz, float a4)
{
    const float m = 69.0f + 12.0f * log2f(hz / a4);
    const int mi = (int)lroundf(m);
    note_t n = {
        .midi = mi,
        .octave = mi / 12 - 1,
        .pc = ((mi % 12) + 12) % 12,
        .cents = (m - mi) * 100.0f,
    };
    return n;
}

/* Letter and whether it's sharp, for pitch class pc. */
static inline char note_letter(int pc)
{
    return "CCDDEFFGGAAB"[pc];
}

static inline int note_sharp(int pc)
{
    return (0x54A >> pc) & 1;   /* C# D# F# G# A# */
}

#define NOTE_SHARP_UTF8 "\xE2\x99\xAF"   /* U+266F */
#define NOTE_FLAT_UTF8 "\xE2\x99\xAD"    /* U+266D */
#define CENT_UTF8 "\xC2\xA2"
#define MIDDOT_UTF8 "\xC2\xB7"
