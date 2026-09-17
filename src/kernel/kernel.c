/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * kernel.c - Kernel entry point.
 * Receives control from the Limine bootloader in 64-bit long mode.
 * The bootloader sets up paging, GDT, and puts us in the higher half.
 */

#include <stdint.h>
#include <stddef.h>
#include "include/limine.h"
#include "include/serial.h"
#include "arch/x86_64/cpu/gdt.h"
#include "arch/x86_64/cpu/idt.h"
#include "arch/x86_64/cpu/apic.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "mm/kheap.h"
#include "drivers/char/console.h"
#include "drivers/timer/pit.h"
#include "drivers/char/mouse.h"
#include "dev/fb/fb.h"
#include "dev/input/evdev.h"
#include "drivers/char/keyboard.h"
#include "drivers/char/shell.h"
#include "tui/tui.h"
#include "drivers/pci/pci.h"
#include "drivers/gpu/display.h"
#include "drivers/net/e1000.h"
#include "drivers/net/pcnet.h"
#include "fs/vfs/vfs.h"
#include "fs/tarfs/tarfs.h"
#include "dev/char/pty.h"
#include "dev/char/pts.h"
#include "net/net.h"
#include "proc/syscall.h"
#include "proc/process.h"
#include "proc/vdso.h"
#include "acpi/acpi.h"

/* Tell Limine what we need at boot by placing requests in a special section. */
static volatile struct limine_framebuffer_request fb_req = {
    .id = LIMINE_FRAMEBUFFER_REQUEST,
    .revision = 0,
};

static volatile struct limine_memmap_request mm_req = {
    .id = LIMINE_MEMMAP_REQUEST,
    .revision = 0,
};

static volatile struct limine_hhdm_request hhdm_req = {
    .id = LIMINE_HHDM_REQUEST,
    .revision = 0,
};

static volatile struct limine_kernel_address_request kaddr_req = {
    .id = LIMINE_KERNEL_ADDRESS_REQUEST,
    .revision = 0,
};

/* Bootloader module request (non-static so other TUs can inspect it). */
volatile struct limine_module_request mod_req = {
    .id = LIMINE_MODULE_REQUEST,
    .revision = 0,
};

static volatile struct limine_rsdp_request rsdp_req = {
    .id = LIMINE_RSDP_REQUEST,
    .revision = 0,
};

/* Halt the CPU on an unrecoverable error. */
static void khalt(void) {
    __asm__ volatile("cli");
    for (;;)
        __asm__ volatile("hlt");
}

/* Print the memory map over serial so we can verify regions. */
static void dump_memmap(struct limine_memmap_response *mm) {
    static const char *type_names[] = {
        "Usable", "Reserved", "ACPI Reclaimable", "ACPI NVS",
        "Bad Memory", "Bootloader Reclaimable", "Kernel/Modules", "Framebuffer"
    };

    serial_puts("[MEM] Memory map:\n");
    for (uint64_t i = 0; i < mm->entry_count; i++) {
        struct limine_memmap_entry *e = mm->entries[i];
        const char *tname = (e->type <= 7) ? type_names[e->type] : "Unknown";
        serial_puts("  [");
        serial_puthex(e->base);
        serial_puts(" - ");
        serial_puthex(e->base + e->length);
        serial_puts("] ");
        serial_puts(tname);
        serial_puts("\n");
    }
}

void kmain(void) {
    serial_init();
    serial_puts("=== Eigen Experimental Kernel ===\n");
    serial_puts("[BOOT] Serial console up.\n");

    /* Step 2: Initialize GDT and IDT */
    gdt_init();
    idt_init();

    /* Verify Limine gave us a framebuffer. */
    if (!fb_req.response || fb_req.response->framebuffer_count == 0) {
        serial_puts("[BOOT] FATAL: No framebuffer from Limine.\n");
        khalt();
    }

    struct limine_framebuffer *fb = fb_req.response->framebuffers[0];
    serial_puts("[FB] Framebuffer: ");
    serial_puthex(fb->width);
    serial_puts("x");
    serial_puthex(fb->height);
    serial_puts(" @ ");
    serial_puthex((uint64_t)fb->address);
    serial_puts("\n");

    /* Step 2b: Initialize Framebuffer Console (white text on pure black) */
    console_init(fb, 0xFFFFFF, 0x000000);
    console_puts("[BOOT] Framebuffer console active.\n");

    /* Dump the memory map via serial. */
    if (mm_req.response)
        dump_memmap(mm_req.response);
    else
        serial_puts("[MEM] Warning: No memory map response from Limine.\n");

    if (hhdm_req.response)  {
        serial_puts("[HHDM] Offset: ");
        serial_puthex(hhdm_req.response->offset);
        serial_puts("\n");
    }

    /* Step 3: Initialize Physical Memory Manager */
    if (mm_req.response && hhdm_req.response) {
        pmm_init(mm_req.response, hhdm_req.response->offset);

        /* Test an allocation to ensure PMM works */
        void *test_page = pmm_alloc_page();
        serial_puts("[PMM] Test allocation page: ");
        serial_puthex((uint64_t)test_page);
        serial_puts("\n");

        /* Step 4: Initialize Virtual Memory Manager */
        vmm_init(mm_req.response, kaddr_req.response, hhdm_req.response->offset);

        /* Step 4b: Initialize Kernel Dynamic Heap */
        kheap_init();

        void *test_kmalloc = kmalloc(128);
        serial_puts("[KHEAP] Test kmalloc(128) -> ");
        serial_puthex((uint64_t)test_kmalloc);
        serial_puts("\n");
        kfree(test_kmalloc);

        /* ACPI discovery before drivers need IRQ routing */
        if (rsdp_req.response && rsdp_req.response->address)
            acpi_init(rsdp_req.response->address);
        else
            serial_puts("[ACPI] no RSDP from Limine\n");

        /* Step 7 & 8: Initialize VFS and mount Initramfs */
        vfs_init();
        if (mod_req.response && mod_req.response->module_count > 0) {
            struct limine_file *initramfs = mod_req.response->modules[0];
            serial_puts("[INITRAMFS] Found module: ");
            serial_puts(initramfs->path ? initramfs->path : "unnamed");
            serial_puts(" (size: ");
            serial_puthex(initramfs->size);
            serial_puts(" bytes)\n");

            struct vfs_node *tar_root = tarfs_init(initramfs->address, initramfs->size);
            if (tar_root) {
                vfs_mount_root(tar_root);
            }
        } else {
            serial_puts("[INITRAMFS] Warning: No initramfs module supplied by bootloader.\n");
        }

        /* Mount kernel devfs (null/zero/urandom/tty) over /dev */
        if (vfs_mount_named("/dev", devfs_root(), "devfs", "devfs") == 0) {
            serial_puts("[VFS] devfs mounted at '/dev'\n");
        }
        /* /dev/fb0: same scanout buffer the console paints. Registered after
         * /dev exists; keep the mount quiet unless it fails. */
        fb_init(fb);
        /* Stage the bootloader framebuffer early so the console and the node
         * have something to paint before the probe chain claims its own. */
        display_select_fallback(fb);
        /* A driver may have claimed its framebuffer during display_init().
         * Adopt it now: point fb0 at it and re-anchor the console geometry,
         * otherwise both keep painting the bootloader's description. */
        display_adopt_driver();
        {
            struct display *ad = display_active();
            if (ad && display_driver_owned()) {
                fb_repoint(&ad->mode.fb);
                console_init(&ad->mode.fb, 0xFFFFFF, 0x000000);
            } else if (!ad) {
                display_select_fallback(fb);
            }
        }
        /* evdev input nodes (/dev/input/event*). PS/2 sources for now. */
        evdev_init();
        if (vfs_mount_named("/dev/pts", ptsfs_mount(), "devpts", "devpts") == 0) {
            serial_puts("[VFS] ptsfs mounted at '/dev/pts'\n");
        }

        /* Mount writable tmpfs over /tmp */
        if (vfs_mount_named("/tmp", tmpfs_root(), "tmpfs", "tmpfs") == 0) {
            serial_puts("[VFS] tmpfs mounted at '/tmp'\n");
        }

        /* Writable home dirs: root FS is read-only TarFS */
        if (vfs_mount_named("/home", tmpfs_new_root("home"), "tmpfs", "tmpfs") == 0) {
            serial_puts("[VFS] tmpfs mounted at '/home'\n");
        }
        if (vfs_mount_named("/root", tmpfs_new_root("root"), "tmpfs", "tmpfs") == 0) {
            serial_puts("[VFS] tmpfs mounted at '/root'\n");
        }

        /* Mount procfs at /proc for `ps`, `top`, `tty` */
        if (vfs_mount_named("/proc", procfs_root(), "proc", "proc") == 0) {
            serial_puts("[VFS] procfs mounted at '/proc'\n");
        }

        /* Step 9: Initialize Fast System Call Interface */
        syscall_init();

        /* Build the vDSO image before the first user process maps it */
        vdso_init();
    }

    /* Step 5: Initialize PCI bus subsystem */
    pci_init();

    /* Probe GPU drivers. Runs before any PCI client claims the display
     * adapter so a modeset cannot move the scanout out from under the e1000
     * allocator's feet - the display never shares a BAR with those devices,
     * but ordering drivers by risk keeps surprises away. The console still
     * paints the bootloader mode; the active display replaces it below. */
    display_init();

    /* Route legacy IRQs through IOAPIC when MADT exists */
    apic_init();

    /* Step 6: Initialize Network Drivers (e1000 or AMD PCnet) */
    e1000_init();
    pcnet_init();

    /* Step 7: Initialize Timer and PS/2 Keyboard + Mouse */
    pit_init(100);
    keyboard_init();
    mouse_init();

    /* Enable hardware interrupts */
    __asm__ volatile("sti");
    serial_puts("[BOOT] Hardware interrupts enabled.\n");

    /* Step 8: Initialize Network Stack & DHCP */
    net_init();

    /* Step 8b: Boot into the configured init program (ash by default,
     * /sbin/basis-core once it exists).
     * Selection order: /etc/bootsh (path plus optional arguments, first
     * line) then /etc/shells (first valid entry, '#' comments) then
     * /bin/sh. A kernel-cmdline bootsh= override needs the Limine cmdline
     * request (not wired yet). Exec failure hands off to the recovery TUI
     * with the reason; a normal shell exit still falls back to the kernel
     * shell below. */
    {
        static char shpath[256];
        const char *bootsh = "/bin/sh";
        const char *bargv[8];
        int bargc = 0;
        int bfd = vfs_open("/etc/bootsh", O_RDONLY, 0);
        if (bfd >= 0) {
            int64_t n = vfs_read(bfd, shpath, sizeof(shpath) - 1);
            vfs_close(bfd);
            if (n > 0) {
                shpath[n] = '\0';
                /* First line only; split into path + argv. */
                for (int i = 0; shpath[i]; i++) {
                    if (shpath[i] == '\n') { shpath[i] = '\0'; break; }
                }
                char *p = shpath;
                while (*p == ' ' || *p == '\t') p++;
                while (*p && bargc < 7) {
                    if (*p == '"') {
                        /* Quoted argument: spaces inside survive. */
                        p++;
                        bargv[bargc++] = p;
                        while (*p && *p != '"') p++;
                        if (*p) *p++ = '\0';
                    } else {
                        char *e = p;
                        while (*e && *e != ' ' && *e != '\t') e++;
                        if (*e) *e++ = '\0';
                        bargv[bargc++] = p;
                        p = e;
                    }
                    while (*p == ' ' || *p == '\t') p++;
                }
                if (bargc > 0) bootsh = bargv[0];
            }
        } else {
            int sfd = vfs_open("/etc/shells", O_RDONLY, 0);
            if (sfd >= 0) {
                int64_t n = vfs_read(sfd, shpath, sizeof(shpath) - 1);
                vfs_close(sfd);
                if (n > 0) {
                    shpath[n] = '\0';
                    char *p = shpath;
                    for (;;) {
                        while (*p == ' ' || *p == '\t') p++;
                        if (*p == '\0') break;
                        if (*p == '\n') {
                            p++;
                            continue;
                        }
                        if (*p == '#') {
                            while (*p && *p != '\n') p++;
                            continue;
                        }
                        break;
                    }
                    char *e = p;
                    while (*e && *e != ' ' && *e != '\t' && *e != '\n') e++;
                    if (e > p) {
                        *e = '\0';
                        bootsh = p;
                    }
                }
            }
        }
        const char *app = bootsh;
        for (int i = 0; bootsh[i]; i++) {
            if (bootsh[i] == '/') app = &bootsh[i + 1];
        }
        const char *sargv[9];
        int sargc;
        if (bargc > 0) {
            /* /etc/bootsh gave explicit argv (argv[0] already the path). */
            for (int i = 0; i < bargc; i++) sargv[i] = bargv[i];
            sargv[bargc] = NULL;
            sargc = bargc;
        } else {
            sargv[0] = app;
            sargv[1] = NULL;
            sargc = 1;
        }
        struct process *shproc = process_create_from_elf(bootsh, app, sargc, sargv);
        if (shproc) {
            serial_puts("[BOOT] Launching user shell: ");
            serial_puts(bootsh);
            serial_puts("\n");
            process_run(shproc);
            serial_puts("[BOOT] User shell exited, falling back to kernel shell.\n");
        } else {
            /* Boot target missing (future: basis-core unreachable) — hand
             * off to the recovery console with the reason instead of
             * silently dropping to the kernel shell. */
            static char reason[192];
            const char *prefix = "Boot target missing: ";
            int pos = 0;
            for (int i = 0; prefix[i] && pos < 190; i++) reason[pos++] = prefix[i];
            for (int i = 0; bootsh[i] && pos < 190; i++) reason[pos++] = bootsh[i];
            reason[pos] = '\0';
            serial_puts("[BOOT] ");
            serial_puts(reason);
            serial_puts(", entering recovery console.\n");
            tui_recovery_menu(reason);
        }
    }

    /* Step 9: Launch Interactive Kernel Shell */
    shell_run();

    khalt();
}
