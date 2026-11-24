// SPDX-License-Identifier: MIT
#ifndef SAMBA_H
#define SAMBA_H

#include <stdint.h>

void samba_reset(void);
void samba_byte(uint8_t c);              // feed one byte received over the USB serial port
uint16_t crc16(uint32_t addr, uint32_t len);

// Provided by the USB code (or the host test):
void cdc_write(const uint8_t *data, uint32_t len);
void samba_go(uint32_t addr);

#endif
