/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * tui.c - Whiptail/Dialog style recovery TUI.
 */

#include "tui.h"
#include "drivers/char/console.h"
#include "drivers/char/keyboard.h"
#include "drivers/char/shell.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "include/serial.h"

static void tui_redraw(const char *reason, int selected) {
    console_clear();
    console_puts("+-----------------------------------------------------------------+\n");
    console_puts("|               EIGEN RECOVERY & DIAGNOSTIC TUI                   |\n");
    console_puts("+-----------------------------------------------------------------+\n");
    if (reason && reason[0]) {
        console_puts("|  Reason: ");
        console_puts(reason);
        console_puts("\n");
    } else {
        console_puts("|  Reason: System Diagnostic & Recovery Mode                      |\n");
    }
    console_puts("|                                                                 |\n");

    const char *items[7] = {
        "  [1] View Kernel Log Buffer (dmesg)",
        "  [2] Inspect Page Tables & Memory Mapping",
        "  [3] List Loaded Driver Modules",
        "  [4] Storage & Partition Inspector",
        "  [5] Hex Dump Memory Address",
        "  [6] Minimal Kernel Shell (Raw Commands)",
        "  [7] Reboot System"
    };

    for (int i = 0; i < 7; i++) {
        if (selected == i + 1) {
            console_puts("|   > ");
            console_puts(items[i] + 4);
            console_puts("                           |\n");
        } else {
            console_puts("|     ");
            console_puts(items[i] + 4);
            console_puts("                           |\n");
        }
    }

    console_puts("|                                                                 |\n");
    console_puts("+-----------------------------------------------------------------+\n");
    console_puts("|  [UP/DOWN] Navigate   |  [ENTER] Select   |  [ESC] Power Off     |\n");
    console_puts("+-----------------------------------------------------------------+\n");
}

static void tui_wait_key(void) {
    console_puts("\n[Press any key to return]\n");
    while (!keyboard_has_char()) {}
    keyboard_getchar();
}

static void action_dmesg(void) {
    console_clear();
    console_puts("=== Kernel Log Buffer (dmesg) ===\n");
    console_puts("[BOOT] Serial console up.\n");
    console_puts("[GDT] Loaded & SSE enabled.\n");
    console_puts("[IDT] Loaded (256 gates initialized).\n");
    console_puts("[PMM] Initialized physical memory allocator.\n");
    console_puts("[VMM] 4-level paging initialized and active.\n");
    console_puts("[VFS] Virtual File System initialized.\n");
    console_puts("[TARFS] In-memory archive mounted.\n");
    tui_wait_key();
}

static void action_memory(void) {
    console_clear();
    console_puts("=== Page Tables & Memory Mapping ===\n");
    console_puts("Paging: 4-level paging active (CR3 switched)\n");
    console_puts("HHDM Offset: 0xFFFF800000000000\n");
    console_puts("Kernel Heap: 0xFFFFFFFF90000000\n");
    tui_wait_key();
}

static void action_drivers(void) {
    console_clear();
    console_puts("=== Loaded Driver Modules ===\n");
    console_puts(" - pci      : PCI Bus Driver\n");
    console_puts(" - e1000    : Intel 82540EM Ethernet\n");
    console_puts(" - keyboard : PS/2 Keyboard Driver\n");
    console_puts(" - pit      : PIT Timer (100 Hz)\n");
    console_puts(" - pty/pts  : Pseudoterminal subsystem\n");
    console_puts(" - tarfs    : TarFS Root Archive\n");
    tui_wait_key();
}

static void action_storage(void) {
    console_clear();
    console_puts("=== Storage & Partition Inspector ===\n");
    console_puts("Mount Points:\n");
    console_puts("  /        -> tarfs\n");
    console_puts("  /dev     -> devfs\n");
    console_puts("  /dev/pts -> ptsfs\n");
    console_puts("  /tmp     -> tmpfs\n");
    console_puts("  /home    -> tmpfs\n");
    console_puts("  /root    -> tmpfs\n");
    console_puts("  /proc    -> procfs\n");
    tui_wait_key();
}

static void action_hexdump(void) {
    console_clear();
    console_puts("=== Hex Dump Memory Address ===\n");
    console_puts("Dumping 0xFFFF800000100000:\n");
    uint8_t *ptr = (uint8_t *)0xFFFF800000100000ULL;
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 16; j++) {
            uint8_t b = ptr[i * 16 + j];
            const char *hex = "0123456789ABCDEF";
            console_putc(hex[b >> 4]);
            console_putc(hex[b & 0xF]);
            console_putc(' ');
        }
        console_puts("\n");
    }
    tui_wait_key();
}

void tui_recovery_menu(const char *reason) {
    int selected = 1;
    while (1) {
        tui_redraw(reason, selected);

        char c = keyboard_getchar();
        if (c == '\x1b') {
            if (keyboard_has_char()) {
                char c2 = keyboard_getchar();
                if (c2 == '[') {
                    char c3 = keyboard_getchar();
                    if (c3 == 'A') { // Up
                        selected--;
                        if (selected < 1) selected = 7;
                    } else if (c3 == 'B') { // Down
                        selected++;
                        if (selected > 7) selected = 1;
                    }
                }
            } else {
                console_clear();
                console_puts("[TUI] System halting...\n");
                __asm__ volatile("cli; hlt");
                while (1);
            }
        } else if (c == '\n' || c == '\r') {
            switch (selected) {
                case 1: action_dmesg(); break;
                case 2: action_memory(); break;
                case 3: action_drivers(); break;
                case 4: action_storage(); break;
                case 5: action_hexdump(); break;
                case 6:
                    console_clear();
                    shell_run();
                    break;
                case 7:
                    console_clear();
                    console_puts("[TUI] Rebooting...\n");
                    __asm__ volatile("outb %0, $0x64" : : "a"((uint8_t)0xFE));
                    while (1);
                default: break;
            }
        }
    }
}
