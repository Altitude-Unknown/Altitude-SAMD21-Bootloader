// SPDX-License-Identifier: MIT
#ifndef USB_H
#define USB_H

#include <stdint.h>

void usb_init(void);
int usb_poll(void);     // call from the main loop; 1 if data moved
void usb_control(void); // answer control requests (also called while a bulk transfer waits)

#endif
