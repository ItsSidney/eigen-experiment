/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * Linker script for the Eigen Experimental kernel.
 * Places the kernel in the higher half at 0xFFFFFFFF80200000.
 * Limine maps us there via its higher half kernel feature.
 */

ENTRY(kmain)

SECTIONS {
    . = 0xFFFFFFFF80200000;

    .text : ALIGN(0x1000) {
        *(.text .text.*)
    }

    .rodata : ALIGN(0x1000) {
        /* Limine request structures must be in read-only data. */
        KEEP(*(.requests))
        *(.rodata .rodata.*)
    }

        .data : ALIGN(0x1000) {
        *(.data .data.*)
    }

    /* Shell command table — linker-collected array of shell_cmd structs.
     * Each command in shell.c (and elsewhere) is tagged with the
     * SHELL_CMD() macro, which places a const struct in .shell_commands.
     * The shell iterates from __shell_commands_start to __shell_commands_end. */
    .shell_commands : ALIGN(8) {
        __shell_commands_start = .;
        *(.shell_commands .shell_commands.*)
        __shell_commands_end = .;
    }

    .bss : ALIGN(0x1000) {
        *(COMMON)
        *(.bss .bss.*)
    }
}
