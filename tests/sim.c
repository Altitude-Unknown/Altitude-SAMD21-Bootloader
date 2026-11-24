// A simulated SAMD21G18A for host tests: the bootloader's rd*/wr* calls land here.
// Models flash with the NVM controller's page buffer, erase-row and write-page
// commands (flash bits only go from 1 to 0), RAM, the chip ID and CPUID
// registers bossac reads, the user row, and a reset request through AIRCR.
// SPDX-License-Identifier: MIT

#include "sim.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint8_t sim_flash[0x40000];
uint8_t sim_ram[0x8000];
uint8_t sim_user_row[256];
static uint8_t page_buffer[64];
static uint32_t nvm_addr, nvm_ctrlb, nvm_status;
int sim_resets;
int sim_bad_access;
int sim_erases, sim_page_writes;

void sim_init(void) {
    memset(sim_flash, 0xFF, sizeof sim_flash);
    memset(sim_ram, 0, sizeof sim_ram);
    memset(sim_user_row, 0xFF, sizeof sim_user_row);
    sim_user_row[0] = 0xFA;  // BOOTPROT = 2 (8 KB), like a board with this bootloader installed
    memset(page_buffer, 0xFF, sizeof page_buffer);
    nvm_addr = nvm_ctrlb = nvm_status = 0;
    sim_resets = sim_bad_access = sim_erases = sim_page_writes = 0;
}

static void bad(const char *what, uint32_t addr) {
    fprintf(stderr, "sim: %s at 0x%08X\n", what, addr);
    sim_bad_access++;
}

static void nvm_command(uint32_t value) {
    if ((value & 0xFF00) != 0xA500) {
        nvm_status |= 0x04;
        return;
    }
    uint32_t cmd = value & 0x7F, addr = nvm_addr * 2;
    int protected_region = addr < 0x2000;  // BOOTPROT = 8 KB
    switch (cmd) {
    case 0x02:  // erase row
        if (protected_region || addr >= sizeof sim_flash) {
            nvm_status |= 0x08;
            return;
        }
        memset(sim_flash + (addr & ~255u), 0xFF, 256);
        sim_erases++;
        break;
    case 0x04:  // write page
        if (protected_region || addr >= sizeof sim_flash) {
            nvm_status |= 0x08;
            return;
        }
        for (int i = 0; i < 64; i++) {
            sim_flash[(addr & ~63u) + i] &= page_buffer[i];
        }
        memset(page_buffer, 0xFF, sizeof page_buffer);
        sim_page_writes++;
        break;
    case 0x44:  // page buffer clear
        memset(page_buffer, 0xFF, sizeof page_buffer);
        break;
    default:
        nvm_status |= 0x04;
    }
}

static uint32_t read_reg(uint32_t a) {
    switch (a & ~3u) {
    case 0xE000ED00: return 0x410CC601;  // CPUID: Cortex-M0+
    case 0x41002018: return 0x10010305;  // DSU DID: SAMD21G18A rev D
    case 0x41004000: return 0;
    case 0x41004004: return nvm_ctrlb;
    case 0x41004008: return 4096 | (3u << 16);
    case 0x41004014: return 1;           // INTFLAG.READY
    case 0x41004018: return nvm_status;
    case 0x4100401C: return nvm_addr;
    case 0x41004020: return 0xFFFF;      // nothing locked
    case 0x0080A00C: return 0x12345678;
    case 0x0080A040: return 0x9ABCDEF0;
    case 0x0080A044: return 0x0F1E2D3C;
    case 0x0080A048: return 0x4B5A6978;
    case 0x00806024: return 0xFFFFFFFF;
    default: break;
    }
    bad("read of an unmodelled register", a);
    return 0;
}

uint32_t rd32(uint32_t a) {
    if (a + 4 <= sizeof sim_flash) {
        uint32_t v;
        memcpy(&v, sim_flash + a, 4);
        return v;
    }
    if (a >= 0x20000000 && a + 4 <= 0x20008000) {
        uint32_t v;
        memcpy(&v, sim_ram + (a - 0x20000000), 4);
        return v;
    }
    if (a >= 0x00804000 && a + 4 <= 0x00804100) {
        uint32_t v;
        memcpy(&v, sim_user_row + (a - 0x00804000), 4);
        return v;
    }
    return read_reg(a);
}

uint16_t rd16(uint32_t a) { return (uint16_t)(rd32(a & ~3u) >> ((a & 2) * 8)); }
uint8_t rd8(uint32_t a) { return (uint8_t)(rd32(a & ~3u) >> ((a & 3) * 8)); }

static void write(uint32_t a, uint32_t v, int size) {
    if (a < sizeof sim_flash) {
        if (size != 4 || (a & 3)) {
            bad("narrow write to flash (the page buffer needs 32-bit writes)", a);
            return;
        }
        memcpy(page_buffer + (a & 63), &v, 4);
        return;
    }
    if (a >= 0x20000000 && a + size <= 0x20008000) {
        memcpy(sim_ram + (a - 0x20000000), &v, size);
        return;
    }
    switch (a) {
    case 0x41004000: nvm_command(v); return;
    case 0x41004004: nvm_ctrlb = v; return;
    case 0x41004014: return;
    case 0x41004018: nvm_status &= ~v; return;
    case 0x4100401C: nvm_addr = v; return;
    case 0xE000ED0C:
        if (v == 0x05FA0004) {
            sim_resets++;
            return;
        }
        break;
    default: break;
    }
    bad("write to an unmodelled register", a);
}

void wr32(uint32_t a, uint32_t v) { write(a, v, 4); }
void wr16(uint32_t a, uint16_t v) { write(a, v, 2); }
void wr8(uint32_t a, uint8_t v) { write(a, v, 1); }
