// Altitude SAMD21 Bootloader: vector table and reset handler.
// SPDX-License-Identifier: MIT

#include <stdint.h>

extern uint32_t _stack_top, _etext, _sdata, _edata, _sbss, _ebss;
int main(void);
void SysTick_Handler(void);

void Reset_Handler(void) {
    uint32_t *src = &_etext, *dst = &_sdata;
    while (dst < &_edata) {
        *dst++ = *src++;
    }
    for (dst = &_sbss; dst < &_ebss; dst++) {
        *dst = 0;
    }
    main();
    for (;;) {
    }
}

static void Default_Handler(void) {
    for (;;) {
    }
}

__attribute__((section(".vectors"), used)) static void (*const vectors[16])(void) = {
    (void (*)(void))(&_stack_top),
    Reset_Handler,
    Default_Handler,  // NMI
    Default_Handler,  // HardFault
    0, 0, 0, 0, 0, 0, 0,
    Default_Handler,  // SVCall
    0, 0,
    Default_Handler,  // PendSV
    SysTick_Handler,
};
