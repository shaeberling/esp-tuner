idf_component_register(
    SRCS "main.c" "board.c" "display.c" "audio.c" "pitch.c" "fft_dsp.c" "ui.c"
         "fonts/font_note_150.c" "fonts/font_acc_64.c" "fonts/font_oct_44.c"
         "fonts/font_num_36.c" "fonts/font_ui_26.c" "fonts/font_ui_20.c" "fonts/font_ui_16.c"
    INCLUDE_DIRS "."
    REQUIRES esp_driver_gpio esp_driver_i2c esp_driver_i2s esp_driver_ledc esp_driver_pcnt
             esp_driver_spi esp_driver_uart esp_lcd esp_adc esp_timer nvs_flash)
# The pitch analysis runs every 21 ms: let the compiler work on it.
set_source_files_properties(pitch.c PROPERTIES COMPILE_OPTIONS "-O3;-ffast-math")
