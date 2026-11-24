// Run the bootloader's SAM-BA code on a pseudo-terminal, so the real bossac
// uploader can talk to it on a PC: bossac -U --port=<link> ...
//   samba_pty <link-path> <flash-dump>
// Makes <link-path> a symlink to the pseudo-terminal, serves until bossac resets
// the "chip" (or 30 s pass), then writes the simulated flash to <flash-dump>.
// SPDX-License-Identifier: MIT

#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE
#define _XOPEN_SOURCE 700
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "../src/samba.h"
#include "sim.h"

static int master = -1;

void cdc_write(const uint8_t *data, uint32_t len) {
    while (len) {
        ssize_t n = write(master, data, len);
        if (n <= 0) {
            return;
        }
        data += n;
        len -= (uint32_t)n;
    }
}
void msc_send(const uint8_t *data, uint32_t len) { (void)data, (void)len; }
void samba_go(uint32_t addr) { (void)addr; }

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: samba_pty <link-path> <flash-dump>\n");
        return 2;
    }
    sim_init();
    master = posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0 || grantpt(master) || unlockpt(master)) {
        perror("pty");
        return 1;
    }
    unlink(argv[1]);
    if (symlink(ptsname(master), argv[1])) {
        perror("symlink");
        return 1;
    }
    struct termios t;
    tcgetattr(master, &t);
    cfmakeraw(&t);
    tcsetattr(master, TCSANOW, &t);
    printf("ready %s\n", ptsname(master));
    fflush(stdout);

    time_t start = time(NULL);
    while (!sim_resets && time(NULL) - start < 30) {
        struct pollfd p = {master, POLLIN, 0};
        if (poll(&p, 1, 100) > 0 && (p.revents & POLLIN)) {
            uint8_t buf[512];
            ssize_t n = read(master, buf, sizeof buf);
            for (ssize_t i = 0; i < n; i++) {
                samba_byte(buf[i]);
            }
        }
    }
    FILE *f = fopen(argv[2], "wb");
    fwrite(sim_flash, 1, sizeof sim_flash, f);
    fclose(f);
    unlink(argv[1]);
    printf("resets=%d bad_access=%d erases=%d page_writes=%d\n", sim_resets, sim_bad_access, sim_erases,
           sim_page_writes);
    return sim_bad_access ? 1 : 0;
}
