#!/bin/sh
# Upload a random "sketch" with the real bossac 1.8 (the Arduino uploader) to
# the bootloader's SAM-BA code running on a simulated SAMD21, using the same
# arguments as the Altitude Arduino board package, and check the flash.
#   tests/bossac_test.sh [path/to/bossac]
# SPDX-License-Identifier: MIT
set -e
cd "$(dirname "$0")"
BOSSAC=${1:-$(ls "$HOME"/Library/Arduino15/packages/adafruit/tools/bossac/*/bossac \
                  "$HOME"/.arduino15/packages/adafruit/tools/bossac/*/bossac 2>/dev/null | head -n 1)}
[ -x "$BOSSAC" ] || { echo "bossac not found; pass its path"; exit 1; }
make -s samba_pty
WORK=$(mktemp -d)
head -c 20000 /dev/urandom > "$WORK/sketch.bin"
for ERASE in "" "-e"; do
    ./samba_pty "$WORK/usbmodemSIM" "$WORK/flash.bin" > "$WORK/pty.log" &
    while [ ! -L "$WORK/usbmodemSIM" ]; do sleep 0.1; done
    "$BOSSAC" --port="$WORK/usbmodemSIM" -U -i --offset=0x2000 $ERASE -w -v "$WORK/sketch.bin" -R | grep -E "Device|Version|Verify successful"
    wait
    python3 - "$WORK" <<'PY'
import sys
w = sys.argv[1]
flash = open(w + "/flash.bin", "rb").read()
sketch = open(w + "/sketch.bin", "rb").read()
assert flash[0x2000:0x2000 + len(sketch)] == sketch, "sketch not written"
assert flash[:0x2000] == b"\xff" * 0x2000, "bootloader area touched"
print("OK: sketch written at 0x2000, bootloader area untouched")
PY
done
rm -rf "$WORK"
