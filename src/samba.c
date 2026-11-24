// Altitude SAMD21 Bootloader: the SAM-BA monitor protocol (what Arduino's bossac uploader speaks).
//
// Commands are ASCII, end with '#', and carry hex numbers: "w00002000,4#".
// Binary mode only (bossac always sends "N#" first). Supported:
//   N  binary mode          V  version              T  terminal mode (answered, not used)
//   w/h/o  read word/halfword/byte     W/H/O  write word/halfword/byte
//   S  receive bytes into memory       R  send bytes from memory
//   G  run code at an address
//   X  erase flash from an address     (Arduino extension)
//   Y  write flash from a RAM buffer   (Arduino extension)
//   Z  CRC-16 of a memory range        (Arduino extension)
// Flash writes below APP_START are refused (the bootloader protects itself).
//
// SPDX-License-Identifier: MIT

#include "samba.h"

#include "board.h"
#include "flash.h"
#include "hw.h"

static char command;          // current command letter, 0 while waiting for one
static uint32_t arg[2];       // the two hex arguments
static uint8_t arg_index;
static uint32_t receive_addr; // 'S': where incoming bytes go
static uint32_t receive_left;
static uint32_t write_source; // 'Y': RAM buffer set by "Y<addr>,0#"

static const char version[] = "v1.1 [Arduino:XYZ] " BOOTLOADER_NAME " " BOOTLOADER_VERSION "\n\r";

static void send(const void *data, uint32_t len) { cdc_write((const uint8_t *)data, len); }

static void send_memory(uint32_t addr, uint32_t len) {
    uint8_t chunk[64];
    while (len) {
        uint32_t n = len < sizeof chunk ? len : sizeof chunk;
        for (uint32_t i = 0; i < n; i++) {
            chunk[i] = rd8(addr + i);
        }
        send(chunk, n);
        addr += n;
        len -= n;
    }
}

uint16_t crc16(uint32_t addr, uint32_t len) {
    // CRC-16/XMODEM (polynomial 0x1021, initial value 0), the checksum bossac expects.
    uint16_t crc = 0;
    while (len--) {
        crc ^= (uint16_t)rd8(addr++) << 8;
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

static void write_from_buffer(uint32_t dest, uint32_t len) {
    uint8_t page[PAGE_SIZE];
    for (uint32_t done = 0; done < len; done += PAGE_SIZE) {
        for (uint32_t i = 0; i < PAGE_SIZE; i++) {
            page[i] = done + i < len ? rd8(write_source + done + i) : 0xFF;
        }
        flash_write_page(dest + done, page);
    }
}

static void run(void) {
    uint32_t a = arg[0], b = arg[1];
    uint8_t out[12];
    switch (command) {
    case 'N':
    case 'T':
        send("\n\r", 2);
        break;
    case 'V':
        send(version, sizeof version - 1);
        break;
    case 'w': {
        uint32_t v = rd32(a);
        out[0] = (uint8_t)v, out[1] = (uint8_t)(v >> 8), out[2] = (uint8_t)(v >> 16), out[3] = (uint8_t)(v >> 24);
        send(out, 4);
        break;
    }
    case 'h': {
        uint16_t v = rd16(a);
        out[0] = (uint8_t)v, out[1] = (uint8_t)(v >> 8);
        send(out, 2);
        break;
    }
    case 'o':
        out[0] = rd8(a);
        send(out, 1);
        break;
    case 'W':
        wr32(a, b);
        break;
    case 'H':
        wr16(a, (uint16_t)b);
        break;
    case 'O':
        wr8(a, (uint8_t)b);
        break;
    case 'S':
        receive_addr = a;
        receive_left = b;
        break;
    case 'R':
        send_memory(a, b);
        break;
    case 'G':
        samba_go(a);
        break;
    case 'X':
        flash_erase_from(a);
        send("X\n\r", 3);
        break;
    case 'Y':
        if (b == 0) {
            write_source = a;
        } else {
            write_from_buffer(a, b);
        }
        send("Y\n\r", 3);
        break;
    case 'Z': {
        uint16_t crc = crc16(a, b);
        static const char hex[] = "0123456789ABCDEF";
        out[0] = 'Z';
        for (int i = 0; i < 8; i++) {
            out[1 + i] = (uint8_t)hex[(crc >> (28 - 4 * i)) & 0xF];
        }
        out[9] = '#', out[10] = '\n', out[11] = '\r';
        send(out, 12);
        break;
    }
    default:
        break;
    }
}

void samba_reset(void) {
    command = 0;
    receive_left = 0;
}

void samba_byte(uint8_t c) {
    if (receive_left) {  // 'S' data goes straight to memory
        wr8(receive_addr++, c);
        receive_left--;
        return;
    }
    if (c == '#') {
        if (command) {
            run();
        }
        command = 0;
        return;
    }
    if (!command) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
            command = (char)c;
            arg[0] = arg[1] = 0;
            arg_index = 0;
        }
        return;  // anything else between commands (newlines, auto-baud bytes) is ignored
    }
    if (c == ',') {
        arg_index = 1;
        return;
    }
    uint32_t digit;
    if (c >= '0' && c <= '9') {
        digit = c - '0';
    } else if (c >= 'A' && c <= 'F') {
        digit = c - 'A' + 10;
    } else if (c >= 'a' && c <= 'f') {
        digit = c - 'a' + 10;
    } else {
        return;
    }
    arg[arg_index] = (arg[arg_index] << 4) | digit;
}
