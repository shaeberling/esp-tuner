#!/usr/bin/env bash
# Regenerates main/fonts/*.c from the TTFs in fonts/src with lv_font_conv.
#   LV_FONT_CONV=/path/to/lv_font_conv fonts/build_fonts.sh
# Defaults to `npx -y lv_font_conv@1.5.3`.
#
# Chakra Petch (OFL) for text and digits, Noto Music (OFL) for the sharp and
# flat signs, which Chakra Petch doesn't have. Only the glyphs the UI shows
# are kept, so even the 150 px note font stays small.
set -euo pipefail
cd "$(dirname "$0")/.."
CONV=${LV_FONT_CONV:-"npx -y lv_font_conv@1.5.3"}
SRC=fonts/src
OUT=main/fonts
mkdir -p "$OUT"

BOLD=$SRC/ChakraPetch-Bold.ttf
SEMI=$SRC/ChakraPetch-SemiBold.ttf
MED=$SRC/ChakraPetch-Medium.ttf
MUSIC=$SRC/NotoMusic-Regular.ttf
ACC=0x266D,0x266F          # flat, sharp

gen() { # name size bpp args...
    local name=$1 size=$2 bpp=$3
    shift 3
    $CONV --no-compress --no-prefilter --bpp "$bpp" --size "$size" --format lvgl \
        --lv-include lvgl.h --lv-font-name "$name" -o "$OUT/$name.c" "$@"
    echo "  $name"
}

echo "fonts -> $OUT"
# The note letter, huge.
gen font_note_150 150 4 --font "$BOLD" --symbols 'ABCDEFG-'
# Its sharp or flat, and the octave beside it.
gen font_acc_64 64 4 --font "$MUSIC" -r $ACC
gen font_oct_44 44 4 --font "$SEMI" --symbols '0123456789-'
# The frequency readout.
gen font_num_36 36 4 --font "$SEMI" --symbols ' 0123456789.,+-Hz'
# Labels: ASCII, cent and middle-dot signs, and the music signs.
gen font_ui_26 26 4 --font "$SEMI" -r 0x20-0x7E,0xA2,0xB7,0xB0 --font "$MUSIC" -r $ACC
gen font_ui_20 20 4 --font "$MED" -r 0x20-0x7E,0xA2,0xB7,0xB0 --font "$MUSIC" -r $ACC
gen font_ui_16 16 4 --font "$MED" -r 0x20-0x7E,0xA2,0xB7
