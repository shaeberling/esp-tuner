#!/usr/bin/env bash
# Flashes build/ onto the SenseCAP Watcher: tools/flash.sh [PORT]
#
# The Watcher's CH342 USB bridge has no flow control and drops bytes when
# esptool sends a whole packet at once, so idf.py flash fails (and leaves
# the board without a bootloader until a flash succeeds). paced_esptool.py,
# from the Muse SDK, sends 64 bytes at a time at the line rate, at 115200.
# The S3's console is the CH342's second port, the one ending in 3.
# Takes about a minute; don't interrupt it.
set -euo pipefail
cd "$(dirname "$0")/.."
PORT=${1:-$(ls /dev/cu.usbmodem*3 2>/dev/null | head -1)}
[ -n "$PORT" ] || { echo "no /dev/cu.usbmodem*3 port: is the Watcher plugged in?" >&2; exit 1; }
[ -f build/flash_args ] || { echo "build first: idf.py build" >&2; exit 1; }
echo "flashing $PORT (paced, 115200 baud)"
cd build
exec python ../tools/paced_esptool.py --chip esp32s3 -p "$PORT" -b 115200 \
    --before default-reset --after hard-reset write-flash "@flash_args"
