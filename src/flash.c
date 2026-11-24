// Altitude SAMD21 Bootloader: writing the application flash.
//
// The NVM controller erases a row (256 bytes) at a time and writes a page
// (64 bytes) at a time from its page buffer, which is filled by ordinary
// 32-bit stores to the page's flash addresses. These functions never touch the
// bootloader's own first 8 KB.
//
// SPDX-License-Identifier: MIT

#include "flash.h"

#include "hw.h"

static int nvm_command(uint32_t cmd, uint32_t addr) {
    while (!(rd8(NVM_INTFLAG) & NVM_INTFLAG_READY)) {
    }
    wr16(NVM_STATUS, NVM_STATUS_ERRORS);  // clear old errors (write 1 to clear)
    wr32(NVM_ADDR, addr >> 1);            // ADDR takes the 16-bit word address
    wr16(NVM_CTRLA, (uint16_t)(NVM_CMDEX | cmd));
    while (!(rd8(NVM_INTFLAG) & NVM_INTFLAG_READY)) {
    }
    return (rd16(NVM_STATUS) & NVM_STATUS_ERRORS) ? -1 : 0;
}

static int allowed(uint32_t addr, uint32_t len) {
    return addr >= APP_START && addr < FLASH_SIZE && len <= FLASH_SIZE - addr;
}

void flash_init(void) {
    // One wait state for 48 MHz, and manual page writes (write only on the WP command).
    wr32(NVM_CTRLB, NVM_CTRLB_RWS(1) | NVM_CTRLB_MANW);
}

int flash_erase_row(uint32_t addr) {
    if (addr % ROW_SIZE || !allowed(addr, ROW_SIZE)) {
        return -1;
    }
    return nvm_command(NVM_CMD_ER, addr);
}

int flash_write_page(uint32_t addr, const uint8_t *data) {
    if (addr % PAGE_SIZE || !allowed(addr, PAGE_SIZE)) {
        return -1;
    }
    nvm_command(NVM_CMD_PBC, addr);
    for (uint32_t i = 0; i < PAGE_SIZE; i += 4) {
        uint32_t word = data[i] | (uint32_t)data[i + 1] << 8 | (uint32_t)data[i + 2] << 16 |
                        (uint32_t)data[i + 3] << 24;
        wr32(addr + i, word);
    }
    return nvm_command(NVM_CMD_WP, addr);
}

int flash_write(uint32_t addr, const uint8_t *data, uint32_t len) {
    // Writes whole pages into already-erased flash; a short last page is padded with 0xFF.
    uint8_t page[PAGE_SIZE];
    if (addr % PAGE_SIZE || !allowed(addr, len)) {
        return -1;
    }
    for (uint32_t done = 0; done < len; done += PAGE_SIZE) {
        for (uint32_t i = 0; i < PAGE_SIZE; i++) {
            page[i] = done + i < len ? data[done + i] : 0xFF;
        }
        if (flash_write_page(addr + done, page)) {
            return -1;
        }
    }
    return 0;
}

int flash_erase_from(uint32_t addr) {
    // Erase every row from addr (rounded down to a row, but never below APP_START) to the end.
    if (addr < APP_START) {
        addr = APP_START;
    }
    for (addr -= addr % ROW_SIZE; addr < FLASH_SIZE; addr += ROW_SIZE) {
        if (flash_erase_row(addr)) {
            return -1;
        }
    }
    return 0;
}
