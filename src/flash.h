// SPDX-License-Identifier: MIT
#ifndef FLASH_H
#define FLASH_H

#include <stdint.h>

void flash_init(void);
int flash_erase_row(uint32_t addr);                                // row aligned, >= APP_START
int flash_write_page(uint32_t addr, const uint8_t *data);          // 64 bytes, page aligned
int flash_write(uint32_t addr, const uint8_t *data, uint32_t len); // whole pages into erased flash
int flash_erase_from(uint32_t addr);                               // erase addr..end of flash

#endif
