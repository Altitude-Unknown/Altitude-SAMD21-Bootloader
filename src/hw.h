// Altitude SAMD21 Bootloader: hardware access.
//
// Register addresses and bit positions are taken from the SAMD21 datasheet and
// cross-checked against Microchip's SAMD21G18A device header (CMSIS-Atmel).
// Only the registers this bootloader uses are listed.
//
// Every register access goes through rd*/wr*. On the chip these are plain
// volatile loads and stores. In the host tests (HOST defined) they call a
// simulated SAMD21, so the flash, SAM-BA and UF2 code can be tested on a PC.
//
// SPDX-License-Identifier: MIT

#ifndef HW_H
#define HW_H

#include <stdint.h>

#ifdef HOST
uint32_t rd32(uint32_t addr);
uint16_t rd16(uint32_t addr);
uint8_t rd8(uint32_t addr);
void wr32(uint32_t addr, uint32_t value);
void wr16(uint32_t addr, uint16_t value);
void wr8(uint32_t addr, uint8_t value);
#else
static inline uint32_t rd32(uint32_t a) { return *(volatile uint32_t *)a; }
static inline uint16_t rd16(uint32_t a) { return *(volatile uint16_t *)a; }
static inline uint8_t rd8(uint32_t a) { return *(volatile uint8_t *)a; }
static inline void wr32(uint32_t a, uint32_t v) { *(volatile uint32_t *)a = v; }
static inline void wr16(uint32_t a, uint16_t v) { *(volatile uint16_t *)a = v; }
static inline void wr8(uint32_t a, uint8_t v) { *(volatile uint8_t *)a = v; }
#endif

// The USB hardware reads and writes RAM buffers behind the compiler's back:
// a barrier stops the compiler caching or reordering memory around those points.
#ifdef HOST
#define barrier() ((void)0)
#else
#define barrier() __asm volatile("" ::: "memory")
#endif

// ---- Memory map -------------------------------------------------------------
#define FLASH_SIZE        0x40000u      // 256 KB (SAMD21x18)
#define PAGE_SIZE         64u
#define ROW_SIZE          256u          // 4 pages: the smallest erasable unit
#define APP_START         0x2000u       // the bootloader owns the first 8 KB
#define RAM_START         0x20000000u
#define RAM_END           0x20008000u   // 32 KB
#define DOUBLE_TAP_ADDR   (RAM_END - 4) // survives a reset; not touched by this bootloader's stack or data
#define DOUBLE_TAP_MAGIC  0xf01669efu   // the value Arduino and UF2 tools use for "stay in the bootloader"

// Factory calibration (NVM software calibration area, OTP4 + 4)
#define CAL_WORD          0x00806024u
#define CAL_USB_TRANSN(w) (((w) >> 13) & 0x1F)
#define CAL_USB_TRANSP(w) (((w) >> 18) & 0x1F)
#define CAL_USB_TRIM(w)   (((w) >> 23) & 0x07)
#define CAL_DFLL_COARSE(w) (((w) >> 26) & 0x3F)

// Serial number: four words (datasheet "Serial Number")
#define SERIAL_WORD0      0x0080A00Cu
#define SERIAL_WORD1      0x0080A040u
#define SERIAL_WORD2      0x0080A044u
#define SERIAL_WORD3      0x0080A048u

// ---- Cortex-M0+ -------------------------------------------------------------
#define SYST_CSR          0xE000E010u
#define SYST_RVR          0xE000E014u
#define SYST_CVR          0xE000E018u
#define SCB_VTOR          0xE000ED08u
#define SCB_AIRCR         0xE000ED0Cu
#define AIRCR_RESET       0x05FA0004u   // VECTKEY | SYSRESETREQ

// ---- PM (power manager) -----------------------------------------------------
#define PM_AHBMASK        0x40000414u
#define PM_APBBMASK       0x4000041Cu
#define PM_AHBMASK_USB    (1u << 6)
#define PM_APBBMASK_USB   (1u << 5)

// ---- SYSCTRL: DFLL48M ---------------------------------------------------------
#define SYSCTRL_PCLKSR    0x4000080Cu
#define SYSCTRL_DFLLCTRL  0x40000824u   // 16-bit
#define SYSCTRL_DFLLVAL   0x40000828u
#define SYSCTRL_DFLLMUL   0x4000082Cu
#define PCLKSR_DFLLRDY    (1u << 4)
#define DFLLCTRL_ENABLE   (1u << 1)
#define DFLLCTRL_MODE     (1u << 2)     // closed loop
#define DFLLCTRL_USBCRM   (1u << 5)     // lock to the USB start-of-frame (no crystal needed)
#define DFLLCTRL_CCDIS    (1u << 8)     // chill cycle off (recommended with USBCRM)

// ---- GCLK ------------------------------------------------------------------------
#define GCLK_STATUS       0x40000C01u   // 8-bit
#define GCLK_CLKCTRL      0x40000C02u   // 16-bit
#define GCLK_GENCTRL      0x40000C04u
#define GCLK_GENDIV       0x40000C08u
#define GCLK_SYNCBUSY     (1u << 7)
#define GENCTRL_SRC_DFLL48M (7u << 8)
#define GENCTRL_GENEN     (1u << 16)
#define GENCTRL_IDC       (1u << 17)
#define CLKCTRL_ID_USB    6u
#define CLKCTRL_GEN(n)    ((uint32_t)(n) << 8)
#define CLKCTRL_CLKEN     (1u << 14)

// ---- NVMCTRL -------------------------------------------------------------------------
#define NVM_CTRLA         0x41004000u   // 16-bit
#define NVM_CTRLB         0x41004004u
#define NVM_INTFLAG       0x41004014u   // 8-bit
#define NVM_STATUS        0x41004018u   // 16-bit
#define NVM_ADDR          0x4100401Cu
#define NVM_CMDEX         0xA500u
#define NVM_CMD_ER        0x02u         // erase row
#define NVM_CMD_WP        0x04u         // write page
#define NVM_CMD_PBC       0x44u         // page buffer clear
#define NVM_CTRLB_RWS(n)  ((uint32_t)(n) << 1)
#define NVM_CTRLB_MANW    (1u << 7)
#define NVM_INTFLAG_READY (1u << 0)
#define NVM_STATUS_ERRORS 0x001Cu       // PROGE | LOCKE | NVME

// ---- PORT (group A) --------------------------------------------------------------------
#define PORTA_DIRSET      0x41004408u
#define PORTA_OUTCLR      0x41004414u
#define PORTA_OUTSET      0x41004418u
#define PORTA_OUTTGL      0x4100441Cu
#define PORTA_PMUX(n)     (0x41004430u + (n))     // one byte per pin pair
#define PORTA_PINCFG(n)   (0x41004440u + (n))     // one byte per pin
#define PINCFG_PMUXEN     (1u << 0)
#define PMUX_G            6u                      // function G: USB on PA24/PA25

// ---- USB (device mode) ---------------------------------------------------------------------
#define USB_BASE          0x41005000u
#define USB_CTRLA         (USB_BASE + 0x00)  // 8-bit
#define USB_SYNCBUSY      (USB_BASE + 0x02)  // 8-bit
#define USB_CTRLB         (USB_BASE + 0x08)  // 16-bit
#define USB_DADD          (USB_BASE + 0x0A)  // 8-bit
#define USB_INTFLAG       (USB_BASE + 0x1C)  // 16-bit
#define USB_DESCADD       (USB_BASE + 0x24)
#define USB_PADCAL        (USB_BASE + 0x28)  // 16-bit
#define USB_EP(n)         (USB_BASE + 0x100 + 0x20 * (n))
#define USB_EPCFG(n)      (USB_EP(n) + 0)
#define USB_EPSTATUSCLR(n) (USB_EP(n) + 4)
#define USB_EPSTATUSSET(n) (USB_EP(n) + 5)
#define USB_EPSTATUS(n)   (USB_EP(n) + 6)
#define USB_EPINTFLAG(n)  (USB_EP(n) + 7)

#define CTRLA_SWRST       (1u << 0)
#define CTRLA_ENABLE      (1u << 1)
#define CTRLB_DETACH      (1u << 0)
#define DADD_ADDEN        (1u << 7)
#define INTFLAG_EORST     (1u << 3)
#define EPTYPE_CONTROL    1u
#define EPTYPE_BULK       3u
#define EPTYPE_INTERRUPT  4u
#define EPCFG(out_type, in_type) ((uint8_t)((out_type) | ((in_type) << 4)))
#define EPSTATUS_DTGLOUT  (1u << 0)
#define EPSTATUS_DTGLIN   (1u << 1)
#define EPSTATUS_STALLRQ0 (1u << 4)
#define EPSTATUS_STALLRQ1 (1u << 5)
#define EPSTATUS_BK0RDY   (1u << 6)
#define EPSTATUS_BK1RDY   (1u << 7)
#define EPINT_TRCPT0      (1u << 0)
#define EPINT_TRCPT1      (1u << 1)
#define EPINT_RXSTP       (1u << 4)
#define EPINT_STALL0      (1u << 5)
#define EPINT_STALL1      (1u << 6)
#define PCKSIZE_COUNT_MASK 0x3FFFu
#define PCKSIZE_MULTI(n)  ((uint32_t)(n) << 14)
#define PCKSIZE_SIZE_64   (3u << 28)
#define PCKSIZE_AUTO_ZLP  (1u << 31)

#endif
