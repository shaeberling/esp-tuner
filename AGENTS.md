# AGENTS.md

Notes for coding agents working on this repository. The README covers what
the tuner does and how it works; this file covers what is easy to get wrong.

## The project

ESP-IDF v6.1 firmware (C11) for the Seeed SenseCAP Watcher (ESP32-S3): a
chromatic tuner with an LVGL 9 UI on a 412×412 round display. One app, no
Wi-Fi, no camera.

| | |
|---|---|
| `main/pitch.c` | pitch detection. Pure C, no ESP-IDF headers outside `#ifdef ESP_PLATFORM`; also builds on the host |
| `main/fft_dsp.c` | the FFT on the device (esp-dsp); `test/fft_host.c` is the host version behind the same `fft.h` |
| `main/audio.c` | I2S + codecs, the capture/analysis task (core 1), test tones |
| `main/ui.c` | the whole screen, driven by a 33 ms LVGL timer that pulls `audio_latest()` |
| `main/display.c` | panel, touch, LVGL task (core 0), the display lock |
| `main/board.c` | IO expander rails, wheel, backlight, battery, power off |
| `main/main.c` | startup, input loop, serial console commands |

## Build and test

```sh
. $IDF_PATH/export.sh          # ESP-IDF v6.1
idf.py build                   # must finish without warnings
make -C test run               # host tests for pitch.c; must print "all passed"
```

Run the host tests after any change to `pitch.c`, `pitch.h` or `fft.h`.
Configuration lives in `sdkconfig.defaults`; `sdkconfig` is generated and not
tracked. After you change the defaults, delete `sdkconfig` before building.

## Flashing: read this first

- **Flash only with `tools/flash.sh`.** `idf.py flash` and plain esptool fail
  on the Watcher's CH342 USB bridge, and a failed flash leaves the board
  without a bootloader. `flash.sh` uses `tools/paced_esptool.py` at 115200.
  It takes about a minute; never interrupt it, and don't hold the port open
  in another process.
- **The ESP32-S3 is the bridge's second port** (`/dev/cu.usbmodem…3` on
  macOS). The first one belongs to the camera's Himax chip.
- **Never erase the whole flash** (`erase-flash`, `erase_flash`,
  `idf.py erase-flash`) and never write to `0x9000–0x3AFFF`: the Watcher's
  factory data (`nvsfactory`) lives there and can't be restored. The
  partition table at `0x8000` deliberately starts the app's partitions at
  `0x3B000`. Keep it that way when you edit `partitions.csv`.

## Talking to the device

- Use `tools/console.py`, not `idf.py monitor`. It clears HUPCL and keeps DTR
  low, so opening or closing the port doesn't reset the chip. If DTR is high
  during a reset, the chip boots into download mode.
- `tools/console.py "<command>" -t <seconds>` sends a console command and
  prints the output. `--reset` reboots and shows the boot log.
- Device output uses CRLF line endings; normalize them before parsing.
- On a sandboxed agent, serial access usually needs the sandbox lifted.

## Verifying changes on the device

You can't hear or see the device, but you can still check most things:

- **Pitch:** `tools/console.py sweep -t 25` plays G3…E6 on the speaker and
  logs what the mic measured. 330–1318 Hz should read within about ±0.7
  cents (most ±0.05). The speaker is too weak below about 300 Hz, so treat
  196/247 Hz results as speaker limits, not tuner bugs.
- **UI:** `tools/snap.py out.png [--before "tone 440 3000 60" --wait 1.5]`
  saves a PNG of the screen. Look at it.
- **Performance:** the `stat` command prints frame rate, LVGL load, heap and
  analysis time. Currently about 6 ms per analysis (it runs every 21 ms),
  about 45 fps, and about 40 KB of internal heap free. DMA buffers and task
  stacks need internal RAM, so watch that number when you add buffers.
- **Recordings:** `tools/rec.py take.wav --tone 440`, then
  `test/analyze_wav take.wav 440` replays a real mic recording through
  `pitch.c` on the host.
- The wheel, touch and power off can only be checked by a person. Say so
  instead of claiming they work.

## Hardware facts that cost time before

- **The mic is the RIGHT I2S slot** (`MIC_SLOT 1` in `audio.c`). The left
  slot is the speaker codec's echo-cancellation loopback: a test tone shows
  up there as a perfect digital copy, so a "mic" test on the left slot
  passes while the mic hears nothing. Don't auto-detect the slot.
- **Power rails** sit on a PCA9535 expander (I2C0, 0x21). The SYSTEM rail
  bit keeps the board alive on battery; `board_init()` keeps it on through a
  software reset. Don't clear it except in `board_power_off()`.
- **The SPD2010 panel** needs draw areas aligned to 4 columns (see
  `round_area` in `display.c`) and RGB565 byte-swapped.
- **Touch I2C errors** in the first ~1.4 s after boot (`SPD2010:
  tp_read_data ... failed`) are normal while the touch controller starts up.
- **A wrong note at the right frequency** is almost always the reference:
  check the A4 value on the screen (amber when it isn't 440) before
  suspecting `pitch.c`.

## Code rules

- **Threads:** LVGL runs only on its task (core 0). Any `lv_*` call from
  another task must hold `display_lock()`. The `ui_*` functions take the lock
  themselves; call those from `main.c` and `audio.c`.
- **Float, not double, in the hot path.** The S3's FPU is single precision;
  `double` is emulated in software and was a large part of an early 13.5 ms
  analysis time.
  `pitch.c` builds with `-O3 -ffast-math`; use `float` literals (`1.0f`) and
  `f` math functions (`log2f`, `exp2f`). `exp10f` doesn't exist on macOS's
  libc, so don't use it.
- **The console task** has a 16 KB stack because `snap` renders the whole
  screen on it. Keep large buffers static or on the heap.
- **Expensive LVGL effects:** a full-screen shadow once dropped the UI to 8
  fps. Prefer arcs or small shadows, and check `stat` after visual changes.
- **Fonts are subsets.** `font_note_150` only has `ABCDEFG-`, `font_num_36`
  only digits and `.,+-Hz`, and so on. Before you show new characters, add
  them in `fonts/build_fonts.sh` and run it (needs `lv_font_conv`, fetched
  with npx).
- **Style:** 4-space indentation, `snake_case`, module prefixes (`board_`,
  `audio_`, `pitch_`, `ui_`, `display_`), file-level state in `static`
  structs. Comments say why, not what. Keep the README's numbers and tables
  in step with the code when you change behavior.
