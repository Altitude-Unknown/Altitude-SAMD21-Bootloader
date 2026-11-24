// Altitude SAMD21 Bootloader: the virtual USB drive.
//
// The drive is an 8 MB FAT16 disk that exists only as answers to sector reads:
// nothing is stored. It shows three files:
//   INFO_UF2.TXT  bootloader version, board name and Board-ID
//   INDEX.HTM     a link to the project
//   CURRENT.UF2   the program now on the board, as a UF2 file
// Copying a .uf2 file to the drive writes it: every sector the computer writes
// is checked for a UF2 block (Microsoft's UF2 format: 512-byte blocks that each
// carry 256 bytes of flash data and their address), and valid blocks for this
// chip are written to flash. When every block of the file has arrived, the
// board restarts into the new program.
//
// SPDX-License-Identifier: MIT

#include "disk.h"

#include "board.h"
#include "flash.h"
#include "hw.h"

#define SECTOR_SIZE     512u
#define TOTAL_SECTORS   16384u   // 8 MB
#define FAT_SECTORS     64u      // each FAT; enough for the 16251 data clusters
#define ROOT_SECTORS    4u       // 64 directory entries
#define FAT1_START      1u
#define ROOT_START      (FAT1_START + 2 * FAT_SECTORS)
#define DATA_START      (ROOT_START + ROOT_SECTORS)

#define UF2_MAGIC0      0x0A324655u
#define UF2_MAGIC1      0x9E5D5157u
#define UF2_MAGIC_END   0x0AB16F30u
#define UF2_NOT_MAIN    0x00000001u
#define UF2_HAS_FAMILY  0x00002000u
#define UF2_FAMILY_SAMD21 0x68ed2b88u
#define UF2_PAYLOAD     256u
#define CURRENT_BLOCKS  ((FLASH_SIZE - APP_START) / UF2_PAYLOAD)
#define MAX_BLOCKS      (FLASH_SIZE / UF2_PAYLOAD)

static const char info_text[] =
    BOOTLOADER_NAME " " BOOTLOADER_VERSION "\r\n"
    "Model: " PRODUCT_NAME "\r\n"
    "Board-ID: " BOARD_ID "\r\n";
static const char index_text[] =
    "<!doctype html>\n<html><body><script>\nlocation.replace(\"" INDEX_URL "\");\n</script></body></html>\n";

struct file {
    char name[12];         // 8.3, space padded (11 characters used)
    uint16_t cluster;      // first cluster; each file is contiguous
    uint32_t size;
};

static const struct file files[] = {
    {"INFO_UF2TXT", 2, sizeof info_text - 1},
    {"INDEX   HTM", 3, sizeof index_text - 1},
    {"CURRENT UF2", 4, CURRENT_BLOCKS * SECTOR_SIZE},
};
#define FILE_COUNT (sizeof files / sizeof files[0])

static uint8_t written[MAX_BLOCKS / 8];  // which blocks of the current UF2 file have arrived
static uint32_t written_count;
static uint32_t expected_blocks;
volatile uint8_t disk_done;              // set when a whole UF2 file has been written

static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, v), put16(p + 2, v >> 16); }
static uint32_t get32(const uint8_t *p) { return p[0] | p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static void copy(uint8_t *dst, const void *src, uint32_t n) {
    const uint8_t *s = src;
    while (n--) {
        *dst++ = *s++;
    }
}

static void boot_sector(uint8_t *b) {
    static const uint8_t head[] = {0xEB, 0x3C, 0x90, 'A', 'L', 'T', 'I', 'T', 'U', 'D', 'E'};
    copy(b, head, sizeof head);
    put16(b + 11, SECTOR_SIZE);
    b[13] = 1;                    // sectors per cluster
    put16(b + 14, 1);             // reserved sectors
    b[16] = 2;                    // FATs
    put16(b + 17, ROOT_SECTORS * SECTOR_SIZE / 32);
    put16(b + 19, TOTAL_SECTORS);
    b[21] = 0xF8;                 // fixed disk
    put16(b + 22, FAT_SECTORS);
    put16(b + 24, 1);             // sectors per track
    put16(b + 26, 1);             // heads
    b[36] = 0x80;                 // drive number
    b[38] = 0x29;                 // extended boot signature
    put32(b + 39, 0x00A5C0DE);    // volume serial number
    copy(b + 43, VOLUME_LABEL "           ", 11);
    copy(b + 54, "FAT16   ", 8);
    b[510] = 0x55, b[511] = 0xAA;
}

static uint16_t fat_entry(uint32_t cluster) {
    if (cluster < 2) {
        return cluster == 0 ? 0xFFF8 : 0xFFFF;
    }
    for (uint32_t i = 0; i < FILE_COUNT; i++) {
        uint32_t clusters = (files[i].size + SECTOR_SIZE - 1) / SECTOR_SIZE;
        uint32_t first = files[i].cluster, last = first + clusters - 1;
        if (cluster >= first && cluster <= last) {
            return cluster == last ? 0xFFFF : (uint16_t)(cluster + 1);
        }
    }
    return 0;  // free
}

static void dir_entry(uint8_t *e, const char *name, uint8_t attr, uint16_t cluster, uint32_t size) {
    copy(e, name, 11);
    e[11] = attr;
    put16(e + 22, 0x6000);        // 12:00
    put16(e + 24, 0x5D77);        // 2026-11-23 (a fixed date; the drive is generated, not stored)
    put16(e + 16, 0x5D77);
    put16(e + 18, 0x5D77);
    put16(e + 26, cluster);
    put32(e + 28, size);
}

static void current_uf2_block(uint32_t index, uint8_t *b) {
    uint32_t addr = APP_START + index * UF2_PAYLOAD;
    put32(b + 0, UF2_MAGIC0);
    put32(b + 4, UF2_MAGIC1);
    put32(b + 8, UF2_HAS_FAMILY);
    put32(b + 12, addr);
    put32(b + 16, UF2_PAYLOAD);
    put32(b + 20, index);
    put32(b + 24, CURRENT_BLOCKS);
    put32(b + 28, UF2_FAMILY_SAMD21);
    for (uint32_t i = 0; i < UF2_PAYLOAD; i++) {
        b[32 + i] = rd8(addr + i);
    }
    put32(b + 508, UF2_MAGIC_END);
}

void disk_read(uint32_t lba, uint8_t *b) {
    for (uint32_t i = 0; i < SECTOR_SIZE; i++) {
        b[i] = 0;
    }
    if (lba == 0) {
        boot_sector(b);
    } else if (lba < ROOT_START) {  // either FAT: both copies are the same
        uint32_t first = ((lba - FAT1_START) % FAT_SECTORS) * (SECTOR_SIZE / 2);
        for (uint32_t i = 0; i < SECTOR_SIZE / 2; i++) {
            put16(b + 2 * i, fat_entry(first + i));
        }
    } else if (lba == ROOT_START) {
        dir_entry(b, VOLUME_LABEL "           ", 0x08, 0, 0);
        for (uint32_t i = 0; i < FILE_COUNT; i++) {
            dir_entry(b + 32 * (i + 1), files[i].name, 0x01, files[i].cluster, files[i].size);
        }
    } else if (lba >= DATA_START) {
        uint32_t cluster = lba - DATA_START + 2;
        if (cluster == files[0].cluster) {
            copy(b, info_text, sizeof info_text - 1);
        } else if (cluster == files[1].cluster) {
            copy(b, index_text, sizeof index_text - 1);
        } else if (cluster >= files[2].cluster && cluster < files[2].cluster + CURRENT_BLOCKS) {
            current_uf2_block(cluster - files[2].cluster, b);
        }
    }
}

int disk_write(uint32_t lba, const uint8_t *b) {
    (void)lba;  // UF2 blocks are recognized by content, wherever the computer puts them
    uint32_t flags = get32(b + 8), addr = get32(b + 12), size = get32(b + 16);
    uint32_t number = get32(b + 20), count = get32(b + 24);
    if (get32(b) != UF2_MAGIC0 || get32(b + 4) != UF2_MAGIC1 || get32(b + 508) != UF2_MAGIC_END) {
        return 0;  // not a UF2 block: directory or FAT updates, which are ignored
    }
    if ((flags & UF2_NOT_MAIN) || ((flags & UF2_HAS_FAMILY) && get32(b + 28) != UF2_FAMILY_SAMD21)) {
        return 0;  // for another chip, or not flash data
    }
    if (size != UF2_PAYLOAD || addr % ROW_SIZE || addr < APP_START || addr > FLASH_SIZE - UF2_PAYLOAD ||
        count == 0 || count > MAX_BLOCKS || number >= count) {
        return -1;
    }
    if (count != expected_blocks) {  // a new file: start counting again
        expected_blocks = count;
        written_count = 0;
        for (uint32_t i = 0; i < sizeof written; i++) {
            written[i] = 0;
        }
    }
    if (flash_erase_row(addr) || flash_write(addr, b + 32, UF2_PAYLOAD)) {
        return -1;
    }
    if (!(written[number / 8] & (1u << (number % 8)))) {
        written[number / 8] |= (uint8_t)(1u << (number % 8));
        if (++written_count == expected_blocks) {
            disk_done = 1;
        }
    }
    return 1;
}

uint32_t disk_sectors(void) { return TOTAL_SECTORS; }
