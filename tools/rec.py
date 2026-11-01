#!/usr/bin/env python3
"""Records raw mic samples from the tuner: tools/rec.py OUT.wav [-n SAMPLES] [--tone HZ]

Sends 'rec N [HZ]' (with HZ, the speaker plays that tone meanwhile) and
writes the right-slot mic samples to a 16-bit mono WAV.
"""
import argparse
import base64
import re
import sys
import wave

import console


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('out')
    ap.add_argument('-p', '--port')
    ap.add_argument('-n', type=int, default=48000)
    ap.add_argument('--tone', type=float, default=0)
    a = ap.parse_args()
    s = console.open_port(a.port or console.default_port())
    console.read_for(s, 0.2)
    console.send(s, f'rec {a.n} {a.tone:g}')
    log = console.read_for(s, 30 + a.n * 2 * 4 / 3 * 10 / 115200, until=b'===END===').decode('latin1').replace('\r\n', '\n')
    m = re.search(r'===REC (\d+) (\d+)===\n(.*?)===END===', log, re.S)
    if not m:
        sys.exit('no recording in:\n' + log[-2000:])
    n, fs = int(m.group(1)), int(m.group(2))
    lines = [l.strip() for l in m.group(3).splitlines()]
    raw = base64.b64decode(''.join(l for l in lines if re.fullmatch(r'[A-Za-z0-9+/=]+', l)))
    if len(raw) != 2 * n:
        sys.exit(f'got {len(raw) // 2} samples, expected {n}')
    with wave.open(a.out, 'wb') as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(fs)
        w.writeframes(raw)
    print(f'{a.out}: {n} samples at {fs} Hz')


if __name__ == '__main__':
    main()
