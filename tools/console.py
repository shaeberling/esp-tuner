#!/usr/bin/env python3
"""Talks to the tuner's serial console without resetting it.

  tools/console.py [-p PORT] [-t SECONDS] [COMMAND ...]

Sends each COMMAND (e.g. 'tone 440 2000', 'sweep', 'stat') and prints what
the board logs for SECONDS (default 3). With --reset, resets the board first
and prints its boot log.
"""
import argparse
import glob
import sys
import time

import serial


def open_port(port):
    s = serial.Serial(port, 115200, timeout=0.1)
    try:   # closing drops DTR, and DTR low with RTS high resets the chip: keep them
        import termios
        attrs = termios.tcgetattr(s.fd)
        attrs[2] &= ~termios.HUPCL
        termios.tcsetattr(s.fd, termios.TCSANOW, attrs)
    except (ImportError, AttributeError, OSError):
        pass
    return s


def default_port():
    ports = sorted(glob.glob('/dev/cu.usbmodem*3'))
    if not ports:
        sys.exit('no /dev/cu.usbmodem*3 port: is the Watcher plugged in?')
    return ports[0]


def send(s, line):
    """The CH342 drops bytes from whole packets: a few at a time."""
    data = (line + '\n').encode()
    for i in range(0, len(data), 16):
        s.write(data[i:i + 16])
        s.flush()
        time.sleep(0.01)


def read_for(s, seconds, until=None):
    end = time.time() + seconds
    out = b''
    while time.time() < end:
        out += s.read(65536)
        if until and until in out:
            break
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('-p', '--port')
    ap.add_argument('-t', '--time', type=float, default=3.0)
    ap.add_argument('--reset', action='store_true')
    ap.add_argument('commands', nargs='*')
    a = ap.parse_args()
    s = open_port(a.port or default_port())
    if a.reset:
        s.dtr = False
        s.rts = True
        time.sleep(0.1)
        s.rts = False      # DTR stays low: high here would hold IO0 low (download mode)
    read_for(s, 0.2 if not a.reset else 0)
    if not a.commands:
        sys.stdout.write(read_for(s, a.time).decode('utf-8', 'replace'))
    for c in a.commands:
        send(s, c)
        sys.stdout.write(read_for(s, a.time).decode('utf-8', 'replace'))
        sys.stdout.flush()


if __name__ == '__main__':
    main()
