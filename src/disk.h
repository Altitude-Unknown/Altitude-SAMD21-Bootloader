// SPDX-License-Identifier: MIT
#ifndef DISK_H
#define DISK_H

#include <stdint.h>

void disk_read(uint32_t lba, uint8_t *sector);         // fill one 512-byte sector
int disk_write(uint32_t lba, const uint8_t *sector);   // 1 = UF2 block written, 0 = ignored, -1 = bad block
uint32_t disk_sectors(void);
extern volatile uint8_t disk_done;                     // a whole UF2 file has been written

#endif
