// Altitude SAMD21 Bootloader: start-up decisions, clocks, and the main loop.
//
// At reset the bootloader decides, before changing any clock, whether to run
// the program at 0x2000 or stay in bootloader mode. It stays when:
//   - there is no valid program (an empty board, or the Arduino core erased the
//     first row of the sketch to ask for the bootloader before an upload), or
//   - the reset button was pressed twice quickly (double tap).
// Otherwise it starts the program with the chip exactly as reset left it.
//
// In bootloader mode it runs the clock at 48 MHz from the DFLL, locked to the
// USB start-of-frame (so no crystal is needed), and serves USB: a serial port
// for Arduino uploads and a drive for UF2 files.
//
// SPDX-License-Identifier: MIT

#include "board.h"
#include "disk.h"
#include "flash.h"
#include "hw.h"
#include "usb.h"

volatile uint32_t ms_ticks;

void SysTick_Handler(void) { ms_ticks++; }

void samba_go(uint32_t addr) {
    // SAM-BA "G": run code that the uploader placed in RAM (an "applet"); it returns when done.
    ((void (*)(void))(addr | 1))();
}

static int program_valid(void) {
    uint32_t sp = rd32(APP_START), pc = rd32(APP_START + 4);
    return sp > RAM_START && sp <= RAM_END && (pc & 1) && pc > APP_START && pc < FLASH_SIZE;
}

static void start_program(void) {
    uint32_t sp = rd32(APP_START), pc = rd32(APP_START + 4);
    wr32(SCB_VTOR, APP_START);
    __asm volatile("msr msp, %0\n bx %1" : : "r"(sp), "r"(pc));
}

static void wait_ms_at_reset_clock(uint32_t ms) {
    // At reset the CPU runs at 1 MHz (8 MHz oscillator divided by 8): SysTick counts 1000 per ms.
    wr32(SYST_RVR, 1000 - 1);
    wr32(SYST_CVR, 0);
    wr32(SYST_CSR, 0x5);  // processor clock, enabled, no interrupt
    while (ms) {
        if (rd32(SYST_CSR) & (1u << 16)) {  // COUNTFLAG: one period passed
            ms--;
        }
    }
    wr32(SYST_CSR, 0);
}

static void clock_init(void) {
    flash_init();  // one flash wait state before the CPU goes to 48 MHz

    // Errata: write DFLLCTRL (clearing ONDEMAND) and wait before touching other DFLL registers.
    wr16(SYSCTRL_DFLLCTRL, DFLLCTRL_ENABLE);
    while (!(rd32(SYSCTRL_PCLKSR) & PCLKSR_DFLLRDY)) {
    }
    uint32_t coarse = CAL_DFLL_COARSE(rd32(CAL_WORD));
    if (coarse == 0x3F) {
        coarse = 0x1F;
    }
    wr32(SYSCTRL_DFLLVAL, coarse << 10 | 512);                   // factory coarse value, mid fine value
    wr32(SYSCTRL_DFLLMUL, 7u << 26 | 0x3Fu << 16 | 48000);       // max steps; 48 MHz / 1 kHz USB frames
    while (!(rd32(SYSCTRL_PCLKSR) & PCLKSR_DFLLRDY)) {
    }
    wr16(SYSCTRL_DFLLCTRL, DFLLCTRL_ENABLE | DFLLCTRL_MODE | DFLLCTRL_USBCRM | DFLLCTRL_CCDIS);
    while (!(rd32(SYSCTRL_PCLKSR) & PCLKSR_DFLLRDY)) {
    }

    // Generic clock 0 (the CPU and USB clock) from the DFLL, undivided.
    wr32(GCLK_GENDIV, 0);
    while (rd8(GCLK_STATUS) & GCLK_SYNCBUSY) {
    }
    wr32(GCLK_GENCTRL, 0 | GENCTRL_SRC_DFLL48M | GENCTRL_GENEN | GENCTRL_IDC);
    while (rd8(GCLK_STATUS) & GCLK_SYNCBUSY) {
    }

    wr32(SYST_RVR, 48000 - 1);  // 1 ms ticks at 48 MHz
    wr32(SYST_CVR, 0);
    wr32(SYST_CSR, 0x7);        // processor clock, interrupt, enabled
}

int main(void) {
    volatile uint32_t *tap = (volatile uint32_t *)DOUBLE_TAP_ADDR;

    if (*tap == DOUBLE_TAP_MAGIC) {
        *tap = 0;                       // second press of a double tap: stay
    } else if (program_valid()) {
        *tap = DOUBLE_TAP_MAGIC;        // a reset during the next half second means "double tap"
        wait_ms_at_reset_clock(DOUBLE_TAP_MS);
        *tap = 0;
        start_program();
    }

    clock_init();
    wr32(PORTA_DIRSET, 1u << LED_PIN);
    usb_init();

    uint32_t blink = 0, done_at = 0;
    for (;;) {
        int busy = usb_poll();
        uint32_t now = ms_ticks;
        if (busy || now - blink >= 500) {  // slow blink while waiting, flicker while data moves
            wr32(PORTA_OUTTGL, 1u << LED_PIN);
            blink = now;
        }
        if (disk_done && !done_at) {
            done_at = now + 1;          // let the computer finish writing before restarting
        }
        if (done_at && now - done_at > 500) {
            wr32(SCB_AIRCR, AIRCR_RESET);  // restart into the new program
        }
    }
}
