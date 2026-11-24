// Altitude SAMD21 Bootloader: USB mass storage (Bulk-Only Transport + SCSI).
//
// The computer sends a 31-byte Command Block Wrapper (CBW) holding a SCSI
// command, then data moves in one direction, then the device answers with a
// 13-byte Command Status Wrapper (CSW). The sectors themselves come from and go
// to disk.c. Reference: USB Mass Storage Class Bulk-Only Transport 1.0, and the
// SCSI commands (SPC/SBC) that macOS, Windows and Linux send to a USB drive.
//
// SPDX-License-Identifier: MIT

#include "msc.h"

#include "board.h"
#include "disk.h"

#define CBW_SIGNATURE 0x43425355u  // "USBC"
#define CSW_SIGNATURE 0x53425355u  // "USBS"
#define SECTOR 512u

enum { WAIT_CBW, RECEIVE_WRITE, RECEIVE_DISCARD };

static uint8_t state = WAIT_CBW;
static uint8_t sector[SECTOR] __attribute__((aligned(4)));
static uint32_t filled;          // bytes of `sector` received so far (WRITE10)
static uint32_t lba, blocks;     // WRITE10 progress
static uint32_t tag, expected;   // from the CBW
static uint32_t residue;
static uint8_t failed;
static uint8_t sense_key, sense_asc;

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | p[2] << 8 | p[3]; }
static void put_be32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24), p[1] = (uint8_t)(v >> 16), p[2] = (uint8_t)(v >> 8), p[3] = (uint8_t)v; }
static void put_le32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8), p[2] = (uint8_t)(v >> 16), p[3] = (uint8_t)(v >> 24); }

static void clear(uint8_t *p, uint32_t n) {
    while (n--) {
        *p++ = 0;
    }
}

static void send_csw(void) {
    uint8_t csw[13];
    put_le32(csw, CSW_SIGNATURE);
    put_le32(csw + 4, tag);
    put_le32(csw + 8, residue);
    csw[12] = failed;
    msc_send(csw, 13);
    state = WAIT_CBW;
}

static void reply(const uint8_t *data, uint32_t len) {
    // Send up to what the computer asked for; the rest is the residue.
    if (len > expected) {
        len = expected;
    }
    msc_send(data, len);
    residue = expected - len;
}

static void fail(uint8_t key, uint8_t asc) {
    failed = 1;
    sense_key = key, sense_asc = asc;
}

static void command(const uint8_t *cb, uint8_t data_in) {
    uint8_t *b = sector;
    clear(b, 36);
    switch (cb[0]) {
    case 0x00:  // TEST UNIT READY
    case 0x1E:  // PREVENT ALLOW MEDIUM REMOVAL
    case 0x1B:  // START STOP UNIT
    case 0x2F:  // VERIFY(10)
    case 0x35:  // SYNCHRONIZE CACHE(10)
        break;
    case 0x12: {  // INQUIRY
        static const char ids[] = "Altitude" "UF2 Bootloader  " "1.0 ";
        b[0] = 0x00;  // direct-access block device
        b[1] = 0x80;  // removable
        b[2] = 0x04;  // SPC-2
        b[3] = 0x02;  // response data format
        b[4] = 31;    // additional length
        for (uint32_t i = 0; i < 28; i++) {
            b[8 + i] = (uint8_t)ids[i];
        }
        reply(b, 36);
        return;
    }
    case 0x03:  // REQUEST SENSE
        b[0] = 0x70;
        b[2] = sense_key;
        b[7] = 10;
        b[12] = sense_asc;
        sense_key = sense_asc = 0;
        reply(b, 18);
        return;
    case 0x25:  // READ CAPACITY(10): last block and block size
        put_be32(b, disk_sectors() - 1);
        put_be32(b + 4, SECTOR);
        reply(b, 8);
        return;
    case 0x23:  // READ FORMAT CAPACITIES
        b[3] = 8;
        put_be32(b + 4, disk_sectors());
        b[8] = 0x02;  // formatted media
        b[10] = 0x02; // block length 512 (0x000200)
        reply(b, 12);
        return;
    case 0x1A:  // MODE SENSE(6): no block descriptors, not write protected
        b[0] = 3;
        reply(b, 4);
        return;
    case 0x5A:  // MODE SENSE(10)
        b[1] = 6;
        reply(b, 8);
        return;
    case 0x28: {  // READ(10)
        uint32_t first = be32(cb + 2), count = (uint32_t)cb[7] << 8 | cb[8];
        residue = expected;
        for (uint32_t i = 0; i < count && residue >= SECTOR; i++) {
            disk_read(first + i, sector);
            msc_send(sector, SECTOR);
            residue -= SECTOR;
        }
        return;
    }
    case 0x2A:  // WRITE(10): the data arrives next, in msc_receive()
        lba = be32(cb + 2);
        blocks = (uint32_t)cb[7] << 8 | cb[8];
        filled = 0;
        residue = expected;
        state = blocks ? RECEIVE_WRITE : WAIT_CBW;
        return;
    default:
        fail(0x05, 0x20);  // ILLEGAL REQUEST, INVALID COMMAND OPERATION CODE
        break;
    }
    residue = expected;
    if (expected && data_in) {
        // Commands this drive doesn't know still get their data phase (zeros), then a failed status.
        clear(sector, SECTOR);
        for (uint32_t left = expected; left;) {
            uint32_t n = left < SECTOR ? left : SECTOR;
            msc_send(sector, n);
            left -= n;
        }
    } else if (expected) {
        state = RECEIVE_DISCARD;
    }
}

void msc_reset(void) {
    state = WAIT_CBW;
    filled = 0;
}

int msc_receive(const uint8_t *data, uint32_t len) {
    // Returns 1 when a CSW has been sent (a command finished), else 0.
    if (state == WAIT_CBW) {
        uint32_t signature = data[0] | data[1] << 8 | (uint32_t)data[2] << 16 | (uint32_t)data[3] << 24;
        if (len != 31 || signature != CBW_SIGNATURE) {
            return 0;  // not a CBW: ignore (the computer will reset the drive)
        }
        tag = data[4] | data[5] << 8 | (uint32_t)data[6] << 16 | (uint32_t)data[7] << 24;
        expected = data[8] | data[9] << 8 | (uint32_t)data[10] << 16 | (uint32_t)data[11] << 24;
        failed = 0;
        residue = 0;
        command(data + 15, data[12] & 0x80);
        if (state != WAIT_CBW) {
            return 0;
        }
        send_csw();
        return 1;
    }
    for (uint32_t i = 0; i < len && residue; i++, residue--) {
        if (state == RECEIVE_WRITE) {
            sector[filled++] = data[i];
            if (filled == SECTOR) {
                if (disk_write(lba++, sector) < 0) {
                    fail(0x03, 0x0C);  // MEDIUM ERROR, WRITE ERROR (reported after the data phase)
                }
                filled = 0;
                blocks--;
            }
        }
    }
    if (residue == 0) {
        send_csw();
        return 1;
    }
    return 0;
}
