// SPDX-License-Identifier: MIT
#ifndef SIM_H
#define SIM_H

#include <stdint.h>

#include "../src/hw.h"

extern uint8_t sim_flash[0x40000];
extern uint8_t sim_ram[0x8000];
extern uint8_t sim_user_row[256];
extern int sim_resets, sim_bad_access, sim_erases, sim_page_writes;
void sim_init(void);

#endif
