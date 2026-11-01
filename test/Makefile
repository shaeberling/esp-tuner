# Host tests for the pitch detector: make -C test
CFLAGS ?= -O2 -std=c11 -Wall -Wextra -Wno-unused-parameter
test_pitch: test_pitch.c ../main/pitch.c fft_host.c ../main/pitch.h ../main/fft.h
	$(CC) $(CFLAGS) -I../main -o $@ test_pitch.c ../main/pitch.c fft_host.c -lm
run: test_pitch
	./test_pitch
clean:
	rm -f test_pitch
.PHONY: run clean
analyze_wav: analyze_wav.c ../main/pitch.c fft_host.c ../main/pitch.h
	$(CC) $(CFLAGS) -I../main -o $@ analyze_wav.c ../main/pitch.c fft_host.c -lm
