/* Altitude SAMD21 Bootloader memory layout.
 * Flash: the first 8 KB (the program starts at 0x2000).
 * RAM: only the first 16 KB. bossac puts its upload buffers at 0x20004000 and up,
 * and the double-tap flag lives in the last word of RAM.
 * SPDX-License-Identifier: MIT */

MEMORY
{
    FLASH (rx) : ORIGIN = 0x00000000, LENGTH = 8K
    RAM (rwx)  : ORIGIN = 0x20000000, LENGTH = 16K
}

_stack_top = ORIGIN(RAM) + LENGTH(RAM);

SECTIONS
{
    .text :
    {
        KEEP(*(.vectors))
        *(.text*)
        *(.rodata*)
        . = ALIGN(4);
        _etext = .;
    } > FLASH

    .data : AT(_etext)
    {
        _sdata = .;
        *(.data*)
        . = ALIGN(4);
        _edata = .;
    } > RAM

    .bss (NOLOAD) :
    {
        _sbss = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        _ebss = .;
    } > RAM

    ASSERT(_etext + SIZEOF(.data) <= 8K, "The bootloader is bigger than 8 KB")
    ASSERT(_ebss + 2048 <= _stack_top, "Less than 2 KB left for the stack")
}
