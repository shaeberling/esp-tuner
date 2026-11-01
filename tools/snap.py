#!/usr/bin/env python3
"""Screenshot of the tuner's display: tools/snap.py OUT.png [-p PORT] [--before CMD]

Sends 'snap' on the serial console; the board prints its screen as
run-length encoded RGB565 ((count, pixel) uint16 pairs), base64'd, between
'===SNAP W H===' and '===END==='. Writes a PNG.
"""
import argparse
import base64
import re
import struct
import sys
import time
import zlib

import console


def png(path, w, h, rgb):
    def chunk(t, d):
        c = struct.pack('>I', len(d)) + t + d
        return c + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    rows = b''.join(b'\0' + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
                + chunk(b'IDAT', zlib.compress(rows, 9)) + chunk(b'IEND', b''))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('out')
    ap.add_argument('-p', '--port')
    ap.add_argument('--before', action='append', default=[], help='console command to send first')
    ap.add_argument('--wait', type=float, default=0.5, help='seconds between --before and the snap')
    a = ap.parse_args()
    s = console.open_port(a.port or console.default_port())
    console.read_for(s, 0.2)
    for c in a.before:
        console.send(s, c)
    if a.before:
        time.sleep(a.wait)
    console.read_for(s, 0.05)
    console.send(s, 'snap')
    log = console.read_for(s, 120, until=b'===END===').decode('latin1').replace('\r\n', '\n')
    m = re.search(r'===SNAP (\d+) (\d+)===\n(.*?)===END===', log, re.S)
    if not m:
        sys.exit('no screenshot in:\n' + log[-2000:])
    w, h = int(m.group(1)), int(m.group(2))
    lines = [l.strip() for l in m.group(3).splitlines()]
    raw = base64.b64decode(''.join(l for l in lines if re.fullmatch(r'[A-Za-z0-9+/=]+', l)))
    px = bytearray()
    for i in range(0, len(raw) - 3, 4):
        n, v = struct.unpack_from('<HH', raw, i)
        r, g, b = (v >> 11) & 31, (v >> 5) & 63, v & 31
        px += bytes(((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2))) * n
    if len(px) != w * h * 3:
        sys.exit(f'got {len(px) // 3} pixels, expected {w * h}')
    png(a.out, w, h, bytes(px))
    print(f'{a.out}: {w}x{h}')


if __name__ == '__main__':
    main()
