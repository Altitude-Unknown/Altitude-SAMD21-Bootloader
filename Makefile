# Altitude SAMD21 Bootloader
#   make BOARD=rc_tx        one board (see boards/)
#   make all                every board
#   make test               host tests (no hardware)
# The ARM compiler from the Adafruit SAMD Arduino core is used if it is installed;
# set TOOLCHAIN to use another arm-none-eabi-gcc.
# SPDX-License-Identifier: MIT

VERSION := 1.0.0
BOARDS := rc_tx rc_rx yellowstone borealis
TOOLCHAIN ?= $(firstword $(wildcard $(HOME)/Library/Arduino15/packages/adafruit/tools/arm-none-eabi-gcc/*/bin/) \
                         $(wildcard $(HOME)/.arduino15/packages/adafruit/tools/arm-none-eabi-gcc/*/bin/))
CC := $(TOOLCHAIN)arm-none-eabi-gcc
OBJCOPY := $(TOOLCHAIN)arm-none-eabi-objcopy
SIZE := $(TOOLCHAIN)arm-none-eabi-size

SOURCES := src/startup.c src/main.c src/usb.c src/samba.c src/msc.c src/disk.c src/flash.c
CFLAGS := -mcpu=cortex-m0plus -mthumb -Os -std=c11 -Wall -Wextra -Werror \
          -ffunction-sections -fdata-sections -fno-common -ffreestanding -nostdlib
LDFLAGS := -T src/samd21.ld -Wl,--gc-sections -nostartfiles -nostdlib -lgcc

.PHONY: all test clean
all:
	@for b in $(BOARDS); do $(MAKE) --no-print-directory BOARD=$$b build/$$b/bootloader-$$b-v$(VERSION).bin || exit 1; done

ifdef BOARD
build/$(BOARD)/bootloader-$(BOARD)-v$(VERSION).bin: $(SOURCES) src/*.h boards/$(BOARD).h src/samd21.ld
	@mkdir -p build/$(BOARD)
	$(CC) $(CFLAGS) -include boards/$(BOARD).h $(SOURCES) $(LDFLAGS) -o build/$(BOARD)/bootloader.elf
	$(OBJCOPY) -O binary build/$(BOARD)/bootloader.elf $@
	@$(SIZE) build/$(BOARD)/bootloader.elf
endif

test:
	$(MAKE) -C tests

clean:
	rm -rf build
	$(MAKE) -C tests clean
