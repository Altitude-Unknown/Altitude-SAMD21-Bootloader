// Board settings: the Makefile passes -include boards/<board>.h.
// SPDX-License-Identifier: MIT
#ifndef BOARD_H
#define BOARD_H

#ifndef PRODUCT_NAME
#error "Build with BOARD=<name> (see boards/)."
#endif

#define BOOTLOADER_NAME "Altitude Bootloader"
#define BOOTLOADER_VERSION "1.0.0"
#define VENDOR_NAME "Altitude Unknown"
#define INDEX_URL "https://github.com/Altitude-Unknown"
#define DOUBLE_TAP_MS 500   // how long a second reset press counts as a double tap

#endif
