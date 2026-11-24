// SPDX-License-Identifier: MIT
#ifndef MSC_H
#define MSC_H

#include <stdint.h>

void msc_reset(void);
int msc_receive(const uint8_t *data, uint32_t len);  // one bulk OUT packet

// Provided by the USB code (or the host test): send on the mass-storage bulk IN endpoint.
void msc_send(const uint8_t *data, uint32_t len);

#endif
