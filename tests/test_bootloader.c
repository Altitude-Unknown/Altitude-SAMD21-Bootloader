// Host tests for the bootloader's UF2 drive, mass storage, SAM-BA and flash code.
// Built against tests/sim.c (a simulated SAMD21), with the board settings of rc_tx.
// SPDX-License-Identifier: MIT

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/disk.h"
#include "../src/flash.h"
#include "../src/msc.h"
#include "../src/samba.h"
#include "sim.h"

static int failures, checks;
#define CHECK(cond)                                                              \
    do {                                                                         \
        checks++;                                                                \
        if (!(cond)) {                                                           \
            failures++;                                                          \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                        \
    } while (0)

// ---- capture what the bootloader sends --------------------------------------------------
static uint8_t out[1 << 16];
static uint32_t out_len;
void cdc_write(const uint8_t *data, uint32_t len) { memcpy(out + out_len, data, len), out_len += len; }
void msc_send(const uint8_t *data, uint32_t len) { memcpy(out + out_len, data, len), out_len += len; }
void samba_go(uint32_t addr) { (void)addr; }

static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8), p[2] = (uint8_t)(v >> 16), p[3] = (uint8_t)(v >> 24); }
static uint32_t get32(const uint8_t *p) { return p[0] | p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static void make_program(uint8_t *p, uint32_t len, uint32_t seed) {
    for (uint32_t i = 0; i < len; i++) {
        seed = seed * 1103515245u + 12345u;
        p[i] = (uint8_t)(seed >> 16);
    }
}

static void uf2_block(uint8_t *b, uint32_t addr, const uint8_t *data, uint32_t index, uint32_t count, uint32_t family) {
    memset(b, 0, 512);
    put32(b, 0x0A324655), put32(b + 4, 0x9E5D5157);
    put32(b + 8, family ? 0x2000 : 0);
    put32(b + 12, addr), put32(b + 16, 256), put32(b + 20, index), put32(b + 24, count), put32(b + 28, family);
    memcpy(b + 32, data, 256);
    put32(b + 508, 0x0AB16F30);
}

// ---- the virtual drive ----------------------------------------------------------------------
static void test_disk_layout(void) {
    uint8_t s[512];
    sim_init();
    disk_read(0, s);
    CHECK(s[510] == 0x55 && s[511] == 0xAA);
    CHECK(memcmp(s + 54, "FAT16   ", 8) == 0);
    CHECK(memcmp(s + 43, "RCTXBOOT   ", 11) == 0);
    disk_read(1, s);  // FAT: media, end, INFO and INDEX single clusters, CURRENT.UF2 chained
    CHECK(get32(s) == 0xFFFFFFF8);
    CHECK((s[4] | s[5] << 8) == 0xFFFF && (s[6] | s[7] << 8) == 0xFFFF);
    CHECK((s[8] | s[9] << 8) == 5);
    disk_read(129, s);  // root directory
    CHECK(s[11] == 0x08 && memcmp(s + 32, "INFO_UF2TXT", 11) == 0);
    CHECK(memcmp(s + 96, "CURRENT UF2", 11) == 0 && get32(s + 96 + 28) == 992 * 512);
    disk_read(133, s);  // INFO_UF2.TXT
    CHECK(strstr((char *)s, "Board-ID: SAMD21G18A-Altitude-RC-TX-M0-v0") != NULL);
    sim_flash[0x2000] = 0x42;
    disk_read(133 + 2, s);  // CURRENT.UF2 block 0 carries the program's first bytes
    CHECK(get32(s) == 0x0A324655 && get32(s + 12) == 0x2000 && s[32] == 0x42 && get32(s + 28) == 0x68ed2b88);
}

static void test_uf2_write(void) {
    static uint8_t program[8192];
    uint8_t b[512];
    sim_init();
    disk_done = 0;
    make_program(program, sizeof program, 7);
    uint32_t count = sizeof program / 256;
    for (uint32_t k = 0; k < count; k++) {  // out of order, as a computer may write them
        uint32_t i = (k * 13) % count;
        uf2_block(b, 0x4000 + 256 * i, program + 256 * i, i, count, 0x68ed2b88);
        CHECK(disk_write(200 + k, b) == 1);
        CHECK(disk_done == (k == count - 1));
    }
    CHECK(memcmp(sim_flash + 0x4000, program, sizeof program) == 0);

    uf2_block(b, 0x0000, program, 0, 1, 0x68ed2b88);   // into the bootloader: refused
    CHECK(disk_write(5000, b) == -1);
    CHECK(sim_flash[0] == 0xFF);
    uf2_block(b, 0x2000, program, 0, 1, 0xADA52840);   // another chip family: ignored
    CHECK(disk_write(5001, b) == 0 && sim_flash[0x2000] == 0xFF);
    memset(b, 0, 512);                                  // a FAT or directory update: ignored
    CHECK(disk_write(1, b) == 0);
    CHECK(sim_bad_access == 0);
}

// ---- mass storage ------------------------------------------------------------------------
static void cbw(uint8_t *c, uint32_t tag, uint32_t length, int in, const uint8_t *cb, int cb_len) {
    memset(c, 0, 31);
    put32(c, 0x43425355), put32(c + 4, tag), put32(c + 8, length);
    c[12] = in ? 0x80 : 0;
    c[14] = (uint8_t)cb_len;
    memcpy(c + 15, cb, cb_len);
}

static void test_msc(void) {
    uint8_t c[31], s[512];
    sim_init();
    msc_reset();
    out_len = 0;
    static const uint8_t inquiry[6] = {0x12, 0, 0, 0, 36, 0};
    cbw(c, 1, 36, 1, inquiry, 6);
    CHECK(msc_receive(c, 31) == 1);
    CHECK(out_len == 36 + 13 && memcmp(out + 8, "Altitude", 8) == 0);
    CHECK(get32(out + 36) == 0x53425355 && get32(out + 40) == 1 && out[48] == 0);

    out_len = 0;
    static const uint8_t capacity[10] = {0x25};
    cbw(c, 2, 8, 1, capacity, 10);
    msc_receive(c, 31);
    CHECK(out[3] == (16383 & 0xFF) && out[2] == (16383 >> 8) && out[6] == 0x02);

    out_len = 0;
    static const uint8_t read0[10] = {0x28, 0, 0, 0, 0, 0, 0, 0, 1, 0};
    cbw(c, 3, 512, 1, read0, 10);
    msc_receive(c, 31);
    disk_read(0, s);
    CHECK(out_len == 512 + 13 && memcmp(out, s, 512) == 0 && out[512 + 12] == 0);

    // WRITE(10) of one UF2 block, arriving in 64-byte packets
    static uint8_t program[256];
    make_program(program, 256, 3);
    uf2_block(s, 0x8000, program, 0, 1, 0x68ed2b88);
    static const uint8_t write1[10] = {0x2A, 0, 0, 0, 0x10, 0, 0, 0, 1, 0};
    out_len = 0;
    cbw(c, 4, 512, 0, write1, 10);
    CHECK(msc_receive(c, 31) == 0);
    for (int i = 0; i < 8; i++) {
        CHECK(msc_receive(s + 64 * i, 64) == (i == 7));
    }
    CHECK(out_len == 13 && get32(out + 4) == 4 && out[12] == 0);
    CHECK(memcmp(sim_flash + 0x8000, program, 256) == 0);

    // An unknown command fails, and REQUEST SENSE says why
    out_len = 0;
    static const uint8_t unknown[10] = {0xEE};
    cbw(c, 5, 0, 0, unknown, 10);
    msc_receive(c, 31);
    CHECK(out[12] == 1);
    out_len = 0;
    static const uint8_t sense[6] = {0x03, 0, 0, 0, 18, 0};
    cbw(c, 6, 18, 1, sense, 6);
    msc_receive(c, 31);
    CHECK(out[2] == 0x05 && out[12] == 0x20);
}

// ---- SAM-BA --------------------------------------------------------------------------------
static void samba(const char *text) {
    out_len = 0;
    while (*text) {
        samba_byte((uint8_t)*text++);
    }
}

static uint16_t crc16_ref(const uint8_t *p, uint32_t n) {
    uint16_t crc = 0;
    while (n--) {
        crc ^= (uint16_t)(*p++ << 8);
        for (int i = 0; i < 8; i++) {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

static void test_samba(void) {
    char cmd[64];
    sim_init();
    samba_reset();
    samba("N#");
    CHECK(out_len == 2 && out[0] == '\n');
    samba("V#");
    out[out_len] = 0;
    CHECK(strstr((char *)out, "[Arduino:XYZ]") != NULL);
    samba("w41002018,4#");
    CHECK(out_len == 4 && get32(out) == 0x10010305);

    // S: bytes into RAM; R: back out
    samba("S20004000,00000004#");
    samba_byte(0xDE), samba_byte(0xAD), samba_byte(0xBE), samba_byte(0xEF);
    CHECK(get32(sim_ram + 0x4000) == 0xEFBEADDE);
    samba("R20004000,00000004#");
    CHECK(out_len == 4 && out[0] == 0xDE && out[3] == 0xEF);

    // Y: write a buffer to flash; Z: checksum it
    static uint8_t program[1024];
    make_program(program, sizeof program, 11);
    memcpy(sim_ram + 0x4100, program, sizeof program);
    samba("Y20004100,0#");
    CHECK(out_len == 3 && out[0] == 'Y');
    samba("Y00002400,00000400#");
    CHECK(out[0] == 'Y' && memcmp(sim_flash + 0x2400, program, sizeof program) == 0);
    samba("Z00002400,00000400#");
    snprintf(cmd, sizeof cmd, "Z%08X#\n\r", crc16_ref(program, sizeof program));
    CHECK(out_len == 12 && memcmp(out, cmd, 12) == 0);

    // The bootloader refuses to overwrite itself
    samba("Y20004100,0#");
    samba("Y00000000,00000100#");
    CHECK(sim_flash[0] == 0xFF);
    samba("X00000000#");  // erase "from 0" starts at the program area
    CHECK(out[0] == 'X' && sim_flash[0x2400] == 0xFF);
    CHECK(sim_bad_access == 0);
}

// Write the drive's first sectors and the files to an image, for a mount test on macOS.
static void write_image(const char *path) {
    FILE *f = fopen(path, "wb");
    uint8_t s[512];
    for (uint32_t lba = 0; lba < disk_sectors(); lba++) {
        disk_read(lba, s);
        fwrite(s, 1, 512, f);
    }
    fclose(f);
}

int main(int argc, char **argv) {
    test_disk_layout();
    test_uf2_write();
    test_msc();
    test_samba();
    if (argc > 1) {
        sim_init();
        write_image(argv[1]);
    }
    printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
