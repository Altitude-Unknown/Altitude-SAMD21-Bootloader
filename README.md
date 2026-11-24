# Altitude SAMD21 Bootloader

The USB bootloader for Altitude Unknown's SAMD21 boards: the RC Airplane
Transmitter and Receiver, YELLOWSTONE, and BOREALIS. It lives in the first
8 KB of flash and lets a board be programmed over USB, two ways:

- **From the Arduino IDE** (or `bossac`): a USB serial port speaking SAM-BA,
  with the Arduino extensions (fast erase, buffered write, CRC check).
- **By copying a `.uf2` file** to the USB drive the board shows in bootloader
  mode (for example `RCTXBOOT`). The drive also has `INFO_UF2.TXT` (version and
  board), `INDEX.HTM`, and `CURRENT.UF2` (the program now on the board).

Written from scratch for Altitude Unknown, with no code from other
bootloaders: about 1,500 lines of C (with comments), using only the compiler.

## Getting into the bootloader

The board stays in the bootloader when:

- there is no program on it,
- the Arduino IDE asks for an upload (the Arduino core erases the start of the
  sketch and resets the board), or
- you **double-tap the reset button** (two presses within half a second).

Otherwise it starts the program at `0x2000` straight away (after a half-second
window for the double tap). The status LED (PA17, Arduino D13) blinks slowly in
the bootloader and flickers while data moves. After a UF2 file has been
copied, the board restarts into the new program by itself.

## USB IDs

| Board | USB ID | Drive |
| --- | --- | --- |
| RC Airplane Transmitter | 1209:A5C0 | RCTXBOOT |
| RC Airplane Receiver | 1209:A5C1 | RCRXBOOT |
| YELLOWSTONE | 1209:A5C2 | YSTONEBOOT |
| BOREALIS | 03EB:2402 (lab placeholder, not for distribution) | BOREALBOOT |

The `1209` IDs are free IDs for open-source hardware from
[pid.codes](https://pid.codes). The bootloader and the board's Arduino sketches
use the same ID.

## Building

```sh
make all              # every board, into build/<board>/
make BOARD=rc_tx      # one board (see boards/)
make test             # host tests: no hardware needed
tests/bossac_test.sh  # upload with the real bossac to a simulated chip
```

The build uses `arm-none-eabi-gcc`. If the Adafruit SAMD Arduino core is
installed, its compiler is found automatically; otherwise set `TOOLCHAIN`
(for example `TOOLCHAIN=/usr/bin/`).

Install a bootloader with the
[Altitude SAMD21 Bootloader Flasher](https://github.com/Altitude-Unknown/SAMD21-Bootloader-Flasher)
and an Atmel-ICE. The flasher also sets the chip's BOOTPROT fuse so the first
8 KB can't be overwritten by a sketch.

## How it is organized

| File | What it does |
| --- | --- |
| `src/main.c` | Start-up decision (program, empty board, double tap), 48 MHz clock locked to USB, main loop |
| `src/usb.c` | The USB device: enumeration, descriptors, CDC serial and mass-storage endpoints |
| `src/samba.c` | The SAM-BA commands the Arduino uploader sends |
| `src/msc.c` | USB mass storage (Bulk-Only Transport) and SCSI commands |
| `src/disk.c` | The virtual FAT16 drive and UF2 file writing |
| `src/flash.c` | Erasing and writing flash, never below `0x2000` |
| `src/hw.h` | The SAMD21 registers used, from the datasheet |
| `boards/*.h` | Per-board names, USB ID, and drive label |
| `tests/` | A simulated SAMD21 and tests for everything except the USB hardware driver |

RAM use stays below `0x20004000`, because bossac stages uploads above it, and
the last word of RAM holds the double-tap flag.

## Testing status

Tested without hardware: the UF2 drive mounts in macOS and shows its files,
UF2 writes, mass-storage commands, and SAM-BA, including uploads by the real
`bossac` 1.8 with the Arduino board package's exact arguments. The USB driver
itself (`src/usb.c`) can only be tested on a board.

## License

MIT (see [LICENSE](LICENSE)).

UF2 is Microsoft's open file format for flashing microcontrollers
([specification](https://github.com/microsoft/uf2)). SAM-BA is Microchip's
bootloader protocol; the commands are implemented here from their published
behavior.
