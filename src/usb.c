// Altitude SAMD21 Bootloader: the USB device.
//
// One composite full-speed device with two functions:
//   interfaces 0-1  CDC ACM serial port   (SAM-BA uploads from Arduino / bossac)
//   interface  2    mass storage          (the UF2 drive)
// Endpoints: 0 control, 1 IN interrupt (CDC notifications, never used),
// 2 OUT/IN bulk (CDC data), 3 OUT/IN bulk (mass storage).
//
// Everything is polled from the main loop; nothing runs in interrupts. The
// SAMD21 USB peripheral moves data by DMA between RAM buffers and the bus,
// described per endpoint and bank in `ep` (bank 0 = OUT, bank 1 = IN).
// References: USB 2.0 chapter 9, USB CDC 1.2 / PSTN, the SAMD21 datasheet USB chapter.
//
// SPDX-License-Identifier: MIT

#include "usb.h"

#include "board.h"
#include "hw.h"
#include "msc.h"
#include "samba.h"

#define EP_CDC_NOTIFY 1
#define EP_CDC 2
#define EP_MSC 3
#define EP_COUNT 4
#define TIMEOUT_MS 500

struct bank {
    uint32_t addr;
    uint32_t pcksize;
    uint16_t extreg;
    uint8_t status;
    uint8_t reserved[5];
};

static volatile struct bank ep[EP_COUNT][2] __attribute__((aligned(4)));
static uint8_t ep0_out[64] __attribute__((aligned(4)));
static uint8_t ep0_in[128] __attribute__((aligned(4)));
static uint8_t cdc_out[64] __attribute__((aligned(4)));
static uint8_t cdc_in[64] __attribute__((aligned(4)));
static uint8_t msc_out[64] __attribute__((aligned(4)));
static uint8_t msc_in[512] __attribute__((aligned(4)));
static uint8_t notify[8] __attribute__((aligned(4)));
static uint8_t line_coding[7] = {0x00, 0xC2, 0x01, 0x00, 0, 0, 8};  // 115200 8N1
static uint8_t configured;

// ---- descriptors ---------------------------------------------------------------------

static const uint8_t device_descriptor[18] = {
    18, 1, 0x00, 0x02,          // USB 2.0
    0xEF, 0x02, 0x01,           // composite device using interface association descriptors
    64,                         // EP0 packet size
    USB_VID & 0xFF, USB_VID >> 8, USB_PID & 0xFF, USB_PID >> 8,
    0x00, 0x01,                 // device release 1.00
    1, 2, 3,                    // manufacturer, product, serial number strings
    1,                          // one configuration
};

#define CONFIG_LENGTH 98
static const uint8_t config_descriptor[CONFIG_LENGTH] = {
    9, 2, CONFIG_LENGTH, 0, 3, 1, 0, 0x80, 50,  // 3 interfaces, bus powered, 100 mA
    // CDC: interface association, communication interface, functional descriptors, notify endpoint
    8, 11, 0, 2, 0x02, 0x02, 0x01, 0,
    9, 4, 0, 0, 1, 0x02, 0x02, 0x01, 0,
    5, 0x24, 0x00, 0x10, 0x01,              // header, CDC 1.10
    5, 0x24, 0x01, 0x00, 1,                 // call management
    4, 0x24, 0x02, 0x02,                    // ACM: line coding and control line state
    5, 0x24, 0x06, 0, 1,                    // union: master 0, slave 1
    7, 5, 0x80 | EP_CDC_NOTIFY, 0x03, 8, 0, 16,
    // CDC data interface
    9, 4, 1, 0, 2, 0x0A, 0x00, 0x00, 0,
    7, 5, EP_CDC, 0x02, 64, 0, 0,
    7, 5, 0x80 | EP_CDC, 0x02, 64, 0, 0,
    // Mass storage: SCSI transparent command set, Bulk-Only Transport
    9, 4, 2, 0, 2, 0x08, 0x06, 0x50, 0,
    7, 5, EP_MSC, 0x02, 64, 0, 0,
    7, 5, 0x80 | EP_MSC, 0x02, 64, 0, 0,
};

// ---- timing (SysTick, set up in main.c) ------------------------------------------------

extern volatile uint32_t ms_ticks;

// ---- endpoint helpers -----------------------------------------------------------------------

static void arm_out(int n, uint8_t *buf) {
    ep[n][0].addr = (uint32_t)buf;
    ep[n][0].pcksize = PCKSIZE_SIZE_64 | PCKSIZE_MULTI(64);
    barrier();
    wr8(USB_EPSTATUSCLR(n), EPSTATUS_BK0RDY);  // bank empty: ready to receive
}

static void stall_ep0(void) { wr8(USB_EPSTATUSSET(0), EPSTATUS_STALLRQ0 | EPSTATUS_STALLRQ1); }

static int wait_flag(int n, uint8_t flag) {
    // Wait for a transfer to finish; give up on timeout, bus reset, or (for EP0) a new SETUP.
    uint32_t start = ms_ticks;
    while (!(rd8(USB_EPINTFLAG(n)) & flag)) {
        if (rd16(USB_INTFLAG) & INTFLAG_EORST || ms_ticks - start > TIMEOUT_MS ||
            (n == 0 && rd8(USB_EPINTFLAG(0)) & EPINT_RXSTP)) {
            return -1;
        }
        if (n != 0) {
            usb_control();  // keep answering control requests while a bulk transfer waits
            if (!configured) {
                return -1;  // the bus was reset meanwhile
            }
        }
    }
    wr8(USB_EPINTFLAG(n), flag);
    barrier();
    return 0;
}

static int send_in(int n, uint8_t *buf, const uint8_t *data, uint32_t len, uint32_t zlp) {
    // Multi-packet IN: the hardware splits `len` bytes into 64-byte packets.
    if (data != buf) {
        for (uint32_t i = 0; i < len; i++) {
            buf[i] = data[i];
        }
    }
    ep[n][1].addr = (uint32_t)buf;
    ep[n][1].pcksize = PCKSIZE_SIZE_64 | len | zlp;
    barrier();
    wr8(USB_EPINTFLAG(n), EPINT_TRCPT1);
    wr8(USB_EPSTATUSSET(n), EPSTATUS_BK1RDY);
    return wait_flag(n, EPINT_TRCPT1);
}

static void send_ep0(const uint8_t *data, uint32_t len, uint32_t requested) {
    if (len > requested) {
        len = requested;
    }
    // A short reply that ends on a packet boundary needs a zero-length packet so the host knows it ended.
    uint32_t zlp = (len < requested && len % 64 == 0) ? PCKSIZE_AUTO_ZLP : 0;
    send_in(0, ep0_in, data, len, zlp);
}

static void reset_bulk(int n, uint8_t out_type, uint8_t in_type) {
    wr8(USB_EPCFG(n), EPCFG(out_type, in_type));
    wr8(USB_EPSTATUSCLR(n), EPSTATUS_DTGLOUT | EPSTATUS_DTGLIN | EPSTATUS_STALLRQ0 | EPSTATUS_STALLRQ1);
    wr8(USB_EPSTATUSCLR(n), EPSTATUS_BK1RDY);
}

static void configure(void) {
    reset_bulk(EP_CDC_NOTIFY, 0, EPTYPE_INTERRUPT);
    ep[EP_CDC_NOTIFY][1].addr = (uint32_t)notify;
    reset_bulk(EP_CDC, EPTYPE_BULK, EPTYPE_BULK);
    arm_out(EP_CDC, cdc_out);
    reset_bulk(EP_MSC, EPTYPE_BULK, EPTYPE_BULK);
    arm_out(EP_MSC, msc_out);
    msc_reset();
    samba_reset();
    configured = 1;
}

static void bus_reset(void) {
    wr16(USB_INTFLAG, INTFLAG_EORST);
    wr8(USB_DADD, 0);
    for (int n = 1; n < EP_COUNT; n++) {
        wr8(USB_EPCFG(n), 0);
    }
    wr8(USB_EPCFG(0), EPCFG(EPTYPE_CONTROL, EPTYPE_CONTROL));
    ep[0][0].addr = (uint32_t)ep0_out;
    ep[0][0].pcksize = PCKSIZE_SIZE_64 | PCKSIZE_MULTI(64);
    ep[0][1].addr = (uint32_t)ep0_in;
    ep[0][1].pcksize = PCKSIZE_SIZE_64;
    wr8(USB_EPSTATUSCLR(0), EPSTATUS_BK0RDY | EPSTATUS_BK1RDY | EPSTATUS_STALLRQ0 | EPSTATUS_STALLRQ1);
    wr8(USB_EPINTFLAG(0), 0xFF);
    configured = 0;
}

// ---- string descriptors ------------------------------------------------------------------

static uint32_t string_descriptor(uint8_t index, uint8_t *out) {
    char text[33];
    const char *s = text;
    if (index == 0) {
        out[0] = 4, out[1] = 3, out[2] = 0x09, out[3] = 0x04;  // US English
        return 4;
    } else if (index == 1) {
        s = VENDOR_NAME;
    } else if (index == 2) {
        s = PRODUCT_NAME;
    } else if (index == 3) {  // the chip's unique serial number, as 32 hex digits
        static const uint32_t words[4] = {SERIAL_WORD0, SERIAL_WORD1, SERIAL_WORD2, SERIAL_WORD3};
        static const char hex[] = "0123456789ABCDEF";
        for (int w = 0; w < 4; w++) {
            uint32_t v = rd32(words[w]);
            for (int i = 0; i < 8; i++) {
                text[w * 8 + i] = hex[(v >> (28 - 4 * i)) & 0xF];
            }
        }
        text[32] = 0;
    } else {
        return 0;
    }
    uint32_t n = 0;
    while (s[n] && n < 62) {
        out[2 + 2 * n] = (uint8_t)s[n];
        out[3 + 2 * n] = 0;
        n++;
    }
    out[0] = (uint8_t)(2 + 2 * n);
    out[1] = 3;
    return out[0];
}

// ---- control requests -------------------------------------------------------------------

static void setup(const uint8_t *s) {
    uint8_t type = s[0], request = s[1];
    uint16_t value = (uint16_t)(s[2] | s[3] << 8), index = (uint16_t)(s[4] | s[5] << 8);
    uint16_t length = (uint16_t)(s[6] | s[7] << 8);
    static const uint8_t zero[2] = {0, 0};

    if ((type & 0x60) == 0x00) {  // standard requests
        switch (request) {
        case 6: {  // GET_DESCRIPTOR
            uint8_t kind = value >> 8;
            if (kind == 1) {
                send_ep0(device_descriptor, sizeof device_descriptor, length);
            } else if (kind == 2) {
                send_ep0(config_descriptor, CONFIG_LENGTH, length);
            } else if (kind == 3) {
                uint32_t n = string_descriptor(value & 0xFF, ep0_in);
                if (n) {
                    send_ep0(ep0_in, n, length);
                } else {
                    stall_ep0();
                }
            } else {
                stall_ep0();  // including the device qualifier: this is a full-speed-only device
            }
            return;
        }
        case 5:  // SET_ADDRESS: answer first, then take the address
            if (send_in(0, ep0_in, 0, 0, 0) == 0) {
                wr8(USB_DADD, (uint8_t)(DADD_ADDEN | (value & 0x7F)));
            }
            return;
        case 9:  // SET_CONFIGURATION
            if (value == 1) {
                configure();
            } else {
                configured = 0;
            }
            send_in(0, ep0_in, 0, 0, 0);
            return;
        case 8:  // GET_CONFIGURATION
            ep0_in[0] = configured;
            send_ep0(ep0_in, 1, length);
            return;
        case 0:   // GET_STATUS
        case 10:  // GET_INTERFACE
            send_ep0(zero, request == 0 ? 2 : 1, length);
            return;
        case 1:  // CLEAR_FEATURE: an endpoint halt is cleared and its data toggle reset
            if ((type & 0x1F) == 2) {
                int n = index & 0x0F;
                if (n < EP_COUNT) {
                    wr8(USB_EPSTATUSCLR(n), index & 0x80 ? (EPSTATUS_STALLRQ1 | EPSTATUS_DTGLIN)
                                                         : (EPSTATUS_STALLRQ0 | EPSTATUS_DTGLOUT));
                }
            }
            send_in(0, ep0_in, 0, 0, 0);
            return;
        case 3:   // SET_FEATURE
        case 11:  // SET_INTERFACE
            send_in(0, ep0_in, 0, 0, 0);
            return;
        default:
            break;
        }
    } else if ((type & 0x60) == 0x20) {  // class requests
        switch (request) {
        case 0x20:  // CDC SET_LINE_CODING: 7 bytes of data follow
            wr8(USB_EPSTATUSCLR(0), EPSTATUS_BK0RDY);
            if (wait_flag(0, EPINT_TRCPT0) == 0) {
                for (int i = 0; i < 7; i++) {
                    line_coding[i] = ep0_out[i];
                }
                send_in(0, ep0_in, 0, 0, 0);
            }
            return;
        case 0x21:  // CDC GET_LINE_CODING
            send_ep0(line_coding, 7, length);
            return;
        case 0x22:  // CDC SET_CONTROL_LINE_STATE
        case 0x23:  // CDC SEND_BREAK
            send_in(0, ep0_in, 0, 0, 0);
            return;
        case 0xFE:  // mass storage GET_MAX_LUN: one drive, LUN 0
            send_ep0(zero, 1, length);
            return;
        case 0xFF:  // mass storage Bulk-Only reset
            msc_reset();
            send_in(0, ep0_in, 0, 0, 0);
            return;
        default:
            break;
        }
    }
    stall_ep0();
}

void usb_control(void) {
    if (rd16(USB_INTFLAG) & INTFLAG_EORST) {
        bus_reset();
    }
    uint8_t flags = rd8(USB_EPINTFLAG(0));
    if (flags & EPINT_RXSTP) {
        uint8_t packet[8];
        barrier();
        for (int i = 0; i < 8; i++) {
            packet[i] = ep0_out[i];
        }
        wr8(USB_EPINTFLAG(0), EPINT_RXSTP | EPINT_TRCPT0 | EPINT_TRCPT1 | EPINT_STALL0 | EPINT_STALL1);
        wr8(USB_EPSTATUSCLR(0), EPSTATUS_STALLRQ0 | EPSTATUS_STALLRQ1);
        setup(packet);
        wr8(USB_EPSTATUSCLR(0), EPSTATUS_BK0RDY);  // ready for the status stage and the next SETUP
    } else if (flags & (EPINT_TRCPT0 | EPINT_STALL0 | EPINT_STALL1)) {
        wr8(USB_EPINTFLAG(0), EPINT_TRCPT0 | EPINT_STALL0 | EPINT_STALL1);
        wr8(USB_EPSTATUSCLR(0), EPSTATUS_BK0RDY);
    }
}

// ---- data endpoints ----------------------------------------------------------------------

void cdc_write(const uint8_t *data, uint32_t len) {
    while (len && configured) {
        uint32_t n = len < sizeof cdc_in ? len : sizeof cdc_in;
        if (send_in(EP_CDC, cdc_in, data, n, 0)) {
            return;  // the computer stopped reading: drop the rest
        }
        data += n;
        len -= n;
    }
}

void msc_send(const uint8_t *data, uint32_t len) {
    while (len && configured) {
        uint32_t n = len < sizeof msc_in ? len : sizeof msc_in;
        if (send_in(EP_MSC, msc_in, data, n, 0)) {
            return;
        }
        data += n;
        len -= n;
    }
}

int usb_poll(void) {
    // Returns 1 if there was USB data activity (for the status LED).
    usb_control();
    if (!configured) {
        return 0;
    }
    int activity = 0;
    if (rd8(USB_EPINTFLAG(EP_CDC)) & EPINT_TRCPT0) {
        wr8(USB_EPINTFLAG(EP_CDC), EPINT_TRCPT0);
        barrier();
        uint32_t n = ep[EP_CDC][0].pcksize & PCKSIZE_COUNT_MASK;
        for (uint32_t i = 0; i < n; i++) {
            samba_byte(cdc_out[i]);
        }
        arm_out(EP_CDC, cdc_out);
        activity = 1;
    }
    if (rd8(USB_EPINTFLAG(EP_MSC)) & EPINT_TRCPT0) {
        wr8(USB_EPINTFLAG(EP_MSC), EPINT_TRCPT0);
        barrier();
        uint32_t n = ep[EP_MSC][0].pcksize & PCKSIZE_COUNT_MASK;
        msc_receive(msc_out, n);
        arm_out(EP_MSC, msc_out);
        activity = 1;
    }
    return activity;
}

void usb_init(void) {
    // USB pins: PA24 = D-, PA25 = D+, peripheral function G.
    wr8(PORTA_PMUX(24 / 2), (uint8_t)(PMUX_G | PMUX_G << 4));
    wr8(PORTA_PINCFG(24), PINCFG_PMUXEN);
    wr8(PORTA_PINCFG(25), PINCFG_PMUXEN);

    wr32(PM_APBBMASK, rd32(PM_APBBMASK) | PM_APBBMASK_USB);
    wr32(PM_AHBMASK, rd32(PM_AHBMASK) | PM_AHBMASK_USB);
    wr16(GCLK_CLKCTRL, (uint16_t)(CLKCTRL_ID_USB | CLKCTRL_GEN(0) | CLKCTRL_CLKEN));

    wr8(USB_CTRLA, CTRLA_SWRST);
    while (rd8(USB_SYNCBUSY) & CTRLA_SWRST) {
    }
    // Pad calibration from the factory values (with the datasheet defaults if unprogrammed).
    uint32_t cal = rd32(CAL_WORD);
    uint32_t transn = CAL_USB_TRANSN(cal), transp = CAL_USB_TRANSP(cal), trim = CAL_USB_TRIM(cal);
    if (transn == 0x1F) transn = 5;
    if (transp == 0x1F) transp = 29;
    if (trim == 0x07) trim = 3;
    wr16(USB_PADCAL, (uint16_t)(transp | transn << 6 | trim << 12));

    wr32(USB_DESCADD, (uint32_t)ep);
    wr8(USB_CTRLA, CTRLA_ENABLE);  // device mode, full speed
    while (rd8(USB_SYNCBUSY) & CTRLA_ENABLE) {
    }
    bus_reset();
    wr16(USB_CTRLB, 0);  // clear DETACH: connect to the bus
}
