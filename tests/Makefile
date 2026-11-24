# Host tests: the bootloader's portable code against a simulated SAMD21.
# SPDX-License-Identifier: MIT

CC ?= cc
CFLAGS := -DHOST -std=c11 -Wall -Wextra -Werror -g -include ../boards/rc_tx.h -I../src
SOURCES := ../src/disk.c ../src/msc.c ../src/samba.c ../src/flash.c sim.c

.PHONY: all clean
all: test_bootloader samba_pty
	./test_bootloader disk.img

test_bootloader: test_bootloader.c $(SOURCES) ../src/*.h sim.h
	$(CC) $(CFLAGS) test_bootloader.c $(SOURCES) -o $@

samba_pty: samba_pty.c $(SOURCES) ../src/*.h sim.h
	$(CC) $(CFLAGS) samba_pty.c $(SOURCES) -o $@

clean:
	rm -f test_bootloader samba_pty disk.img
