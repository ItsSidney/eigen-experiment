/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * ac97.c - Intel 82801AA (ICH) AC'97 / DAC97 Audio Controller Driver
 *
 * Supported hardware:
 *   QEMU:       -device AC97 (PCI 8086:2415)
 *   VirtualBox: ICH AC97 Audio
 *   Bochs:      AC97
 *   Real HW:    Intel ICH / ICH0 / ICH2 / ICH3 / ICH4 / ICH5 AC'97 Audio
 */

#include <stdint.h>
#include <stddef.h>
#include "ac97.h"
#include "sound.h"
#include "drivers/pci/pci.h"
#include "drivers/timer/pit.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "arch/x86_64/cpu/idt.h"
#include "arch/x86_64/cpu/apic.h"
#include "include/serial.h"

/* ── Driver state ────────────────────────────────────────────────────────── */
static uint16_t ac97_nambar = 0;   /* Mixer Base I/O Port (BAR0) */
static uint16_t ac97_nabmbar = 0;  /* Bus Master Base I/O Port (BAR1) */
static uint8_t  ac97_irq = 0;
static int      ac97_present = 0;
static uint64_t ac97_hhdm_offset = 0;

/* Buffer Descriptors & Sample Buffers */
static struct ac97_bd *ac97_bdl = NULL;
static uint64_t        ac97_bdl_phys = 0;

static uint8_t *ac97_buffers[AC97_NUM_BUFFERS];
static uint64_t ac97_buf_phys[AC97_NUM_BUFFERS];

/* Ring buffer pointers */
static volatile uint8_t ac97_head = 0;      /* Next buffer to write to */
static volatile uint8_t ac97_tail = 0;      /* Last buffer consumed by DMA */
static volatile int     ac97_running = 0;   /* Is DMA currently playing */

/* DAC settings */
static uint32_t ac97_sample_rate = 48000;
static uint8_t  ac97_channels = 2;
static uint8_t  ac97_bits = 16;
static uint8_t  ac97_vol_left = 100;
static uint8_t  ac97_vol_right = 100;

/* Forward declarations */
static int ac97_set_format(uint32_t rate, uint8_t channels, uint8_t bits);
static int ac97_set_volume(uint8_t left, uint8_t right);
static int ac97_get_volume(uint8_t *left, uint8_t *right);
static int64_t ac97_write_pcm(const void *buf, size_t count);
static size_t ac97_get_free_space(void);
static int ac97_start(void);
static int ac97_stop(void);
static void ac97_drain(void);

static struct sound_card ac97_sound_card = {
    .name = "AC97",
    .driver_name = "IntelICH",
    .long_name = "Intel 82801AA-ICH AC97 Audio / DAC97",
    .set_format = ac97_set_format,
    .set_volume = ac97_set_volume,
    .get_volume = ac97_get_volume,
    .write_pcm = ac97_write_pcm,
    .get_free_space = ac97_get_free_space,
    .start = ac97_start,
    .stop = ac97_stop,
    .drain = ac97_drain,
};

/* ── Helper delay ────────────────────────────────────────────────────────── */
static void ac97_delay_us(uint32_t us) {
    for (uint32_t i = 0; i < us * 5; i++) {
        inb(0x80); /* I/O delay port */
    }
}

/* ── Mixer read/write ────────────────────────────────────────────────────── */
static inline uint16_t ac97_mixer_read(uint8_t reg) {
    return inw(ac97_nambar + reg);
}

static inline void ac97_mixer_write(uint8_t reg, uint16_t val) {
    outw(ac97_nambar + reg, val);
}

/* ── IRQ Handler ─────────────────────────────────────────────────────────── */
static void ac97_irq_handler(struct interrupt_frame *frame) {
    (void)frame;
    if (!ac97_present) return;

    uint16_t sr = inw(ac97_nabmbar + AC97_PO_SR);

    /* Acknowledge and clear status bits */
    outw(ac97_nabmbar + AC97_PO_SR, sr & (AC97_SR_BCIS | AC97_SR_LVBCI | AC97_SR_CELV | AC97_SR_FIFOE));

    /* Update current index */
    ac97_tail = inb(ac97_nabmbar + AC97_PO_CIV);

    /* If halted or current == last valid and no more buffers queued */
    if ((sr & AC97_SR_CELV) || (sr & AC97_SR_DCH)) {
        if (ac97_tail == ac97_head) {
            ac97_running = 0;
        }
    }
}

/* ── Update tail from hardware CIV ───────────────────────────────────────── */
static void ac97_poll_hw_status(void) {
    if (!ac97_present) return;

    uint16_t sr = inw(ac97_nabmbar + AC97_PO_SR);
    ac97_tail = inb(ac97_nabmbar + AC97_PO_CIV);

    /* Clear any pending status bits */
    if (sr & (AC97_SR_BCIS | AC97_SR_LVBCI | AC97_SR_CELV | AC97_SR_FIFOE)) {
        outw(ac97_nabmbar + AC97_PO_SR, sr & (AC97_SR_BCIS | AC97_SR_LVBCI | AC97_SR_CELV | AC97_SR_FIFOE));
    }

    if ((sr & AC97_SR_CELV) || (sr & AC97_SR_DCH)) {
        if (ac97_tail == ac97_head) {
            ac97_running = 0;
        }
    }
}

/* ── Free buffer count in ring ───────────────────────────────────────────── */
static uint8_t ac97_free_buffers(void) {
    ac97_poll_hw_status();
    uint8_t tail = ac97_tail;
    uint8_t head = ac97_head;
    if (head >= tail) {
        return (AC97_NUM_BUFFERS - 1) - (head - tail);
    } else {
        return (tail - head) - 1;
    }
}

static size_t ac97_get_free_space(void) {
    return (size_t)ac97_free_buffers() * AC97_BUFFER_SIZE;
}

/* ── DAC format configuration ────────────────────────────────────────────── */
static int ac97_set_format(uint32_t rate, uint8_t channels, uint8_t bits) {
    if (!ac97_present) return -1;

    ac97_channels = (channels == 1) ? 1 : 2;
    ac97_bits = (bits == 8) ? 8 : 16;

    /* Check Variable Rate Audio (VRA) capability in extended audio ID */
    uint16_t ext_id = ac97_mixer_read(AC97_EXTENDED_ID);
    if (ext_id & 1) {
        /* VRA supported: enable Variable Rate Audio in extended status */
        uint16_t ext_stat = ac97_mixer_read(AC97_EXTENDED_STAT);
        ac97_mixer_write(AC97_EXTENDED_STAT, ext_stat | 1);

        /* Program Front DAC rate */
        ac97_mixer_write(AC97_FRONT_DAC_RATE, (uint16_t)rate);
        ac97_delay_us(100);

        /* Read back actual configured rate */
        uint16_t actual_rate = ac97_mixer_read(AC97_FRONT_DAC_RATE);
        if (actual_rate > 0) {
            ac97_sample_rate = actual_rate;
        } else {
            ac97_sample_rate = rate;
        }

        /* Also program L/R DAC rate if register is present */
        ac97_mixer_write(AC97_LR_DAC_RATE, (uint16_t)ac97_sample_rate);
    } else {
        /* Fixed rate AC'97: hardware runs at 48000 Hz */
        ac97_sample_rate = 48000;
    }

    return 0;
}

/* ── DAC volume control ──────────────────────────────────────────────────── */
static int ac97_set_volume(uint8_t left, uint8_t right) {
    if (!ac97_present) return -1;
    if (left > 100) left = 100;
    if (right > 100) right = 100;

    ac97_vol_left = left;
    ac97_vol_right = right;

    /* In AC97: 0 = 0dB (loudest), 31 = -46.5dB (quietest), 0x8000 = mute */
    uint16_t master_reg = 0;
    if (left == 0 && right == 0) {
        master_reg = 0x8000; /* Mute */
    } else {
        uint8_t atten_l = (uint8_t)((100 - left) * 31 / 100);
        uint8_t atten_r = (uint8_t)((100 - right) * 31 / 100);
        master_reg = (uint16_t)((atten_l << 8) | atten_r);
    }

    /* Set Master Output and Headphone Volume */
    ac97_mixer_write(AC97_MASTER_VOL, master_reg);
    ac97_mixer_write(AC97_HEADPHONE_VOL, master_reg);

    /* Set PCM Out DAC volume: 0x0000 = 0dB gain */
    ac97_mixer_write(AC97_PCM_OUT_VOL, master_reg & 0x1F1F);

    return 0;
}

static int ac97_get_volume(uint8_t *left, uint8_t *right) {
    if (left) *left = ac97_vol_left;
    if (right) *right = ac97_vol_right;
    return 0;
}

/* ── Start/Stop DMA playback ─────────────────────────────────────────────── */
static int ac97_start(void) {
    if (!ac97_present) return -1;
    uint8_t cr = inb(ac97_nabmbar + AC97_PO_CR);
    if (!(cr & AC97_CR_RPBM)) {
        outb(ac97_nabmbar + AC97_PO_CR, AC97_CR_RPBM | AC97_CR_IOCE | AC97_CR_LVBIE);
        ac97_running = 1;
    }
    return 0;
}

static int ac97_stop(void) {
    if (!ac97_present) return -1;
    outb(ac97_nabmbar + AC97_PO_CR, 0);
    ac97_running = 0;
    return 0;
}

static void ac97_drain(void) {
    if (!ac97_present) return;
    int timeout = 500; /* ms */
    while (timeout-- > 0) {
        ac97_poll_hw_status();
        if (ac97_tail == ac97_head && !ac97_running) break;
        timer_sleep_ms(1);
    }
}

/* ── Write PCM samples to AC'97 DMA buffers ──────────────────────────────── */
static int64_t ac97_write_pcm(const void *buf, size_t count) {
    if (!ac97_present || !buf) return -1;
    if (count == 0) return 0;

    const uint8_t *src = (const uint8_t *)buf;
    size_t total_written = 0;

    while (total_written < count) {
        /* Wait for free slot if ring is full */
        int retry = 200;
        while (ac97_free_buffers() == 0) {
            if (--retry <= 0) {
                /* Buffer underrun or hardware stalled: try restarting DMA */
                ac97_start();
                break;
            }
            timer_sleep_ms(1);
        }

        if (ac97_free_buffers() == 0) {
            /* Still full */
            break;
        }

        uint8_t idx = ac97_head;
        uint8_t *dst = ac97_buffers[idx];
        size_t chunk = count - total_written;
        if (chunk > AC97_BUFFER_SIZE) chunk = AC97_BUFFER_SIZE;

        /* Convert format to AC'97 hardware native: 16-bit signed stereo interleaved */
        if (ac97_bits == 16 && ac97_channels == 2) {
            /* Native 16-bit stereo: direct copy */
            for (size_t i = 0; i < chunk; i++) dst[i] = src[i];
            /* If chunk is less than buffer size, pad rest with silence (0) */
            for (size_t i = chunk; i < AC97_BUFFER_SIZE; i++) dst[i] = 0;
            total_written += chunk;
            src += chunk;
        } else if (ac97_bits == 16 && ac97_channels == 1) {
            /* 16-bit mono -> duplicate left to right stereo */
            const int16_t *s16 = (const int16_t *)src;
            int16_t *d16 = (int16_t *)dst;
            size_t samples = chunk / 2;
            size_t max_pairs = AC97_BUFFER_SIZE / 4;
            if (samples > max_pairs) samples = max_pairs;
            for (size_t i = 0; i < samples; i++) {
                int16_t val = s16[i];
                d16[i * 2]     = val;
                d16[i * 2 + 1] = val;
            }
            for (size_t i = samples * 2; i < AC97_BUFFER_SIZE / 2; i++) d16[i] = 0;
            total_written += samples * 2;
            src += samples * 2;
        } else if (ac97_bits == 8 && ac97_channels == 1) {
            /* 8-bit unsigned mono -> 16-bit signed stereo */
            int16_t *d16 = (int16_t *)dst;
            size_t samples = chunk;
            size_t max_pairs = AC97_BUFFER_SIZE / 4;
            if (samples > max_pairs) samples = max_pairs;
            for (size_t i = 0; i < samples; i++) {
                int16_t val = (int16_t)(((int)src[i] - 128) << 8);
                d16[i * 2]     = val;
                d16[i * 2 + 1] = val;
            }
            for (size_t i = samples * 2; i < AC97_BUFFER_SIZE / 2; i++) d16[i] = 0;
            total_written += samples;
            src += samples;
        } else if (ac97_bits == 8 && ac97_channels == 2) {
            /* 8-bit unsigned stereo -> 16-bit signed stereo */
            int16_t *d16 = (int16_t *)dst;
            size_t pairs = chunk / 2;
            size_t max_pairs = AC97_BUFFER_SIZE / 4;
            if (pairs > max_pairs) pairs = max_pairs;
            for (size_t i = 0; i < pairs; i++) {
                d16[i * 2]     = (int16_t)(((int)src[i * 2] - 128) << 8);
                d16[i * 2 + 1] = (int16_t)(((int)src[i * 2 + 1] - 128) << 8);
            }
            for (size_t i = pairs * 2; i < AC97_BUFFER_SIZE / 2; i++) d16[i] = 0;
            total_written += pairs * 2;
            src += pairs * 2;
        }

        /* Each sample word in AC97 BDL is 16 bits (2 bytes). Buffer has 4096 bytes = 2048 samples */
        ac97_bdl[idx].addr = (uint32_t)ac97_buf_phys[idx];
        ac97_bdl[idx].length = (uint16_t)(AC97_BUFFER_SIZE / 2);
        ac97_bdl[idx].flags = AC97_BD_IOC | AC97_BD_BUP;

        /* Advance head */
        uint8_t next_head = (idx + 1) % AC97_NUM_BUFFERS;
        ac97_head = next_head;

        /* Update Last Valid Index in hardware */
        outb(ac97_nabmbar + AC97_PO_LVI, idx);

        /* Ensure DMA is running */
        ac97_start();
    }

    return (int64_t)total_written;
}

/* ── Hardware Probe & Init ───────────────────────────────────────────────── */
int ac97_init(void) {
    if (ac97_present) return 0;

    /* Search PCI bus for Intel AC'97 audio controller or multimedia audio class */
    struct pci_device *pdev = pci_find_device(0x8086, 0x2415); /* Intel 82801AA ICH AC97 */
    if (!pdev) pdev = pci_find_device(0x8086, 0x2425);        /* ICH0 */
    if (!pdev) pdev = pci_find_device(0x8086, 0x2445);        /* ICH2 */
    if (!pdev) pdev = pci_find_device(0x8086, 0x2485);        /* ICH3 */
    if (!pdev) pdev = pci_find_device(0x8086, 0x24C5);        /* ICH4 */
    if (!pdev) pdev = pci_find_device(0x8086, 0x24D5);        /* ICH5 */
    if (!pdev) pdev = pci_find_device(0x8086, 0x266E);        /* ICH6 */
    if (!pdev) pdev = pci_find_device(0x1022, 0x7445);        /* AMD AC97 */
    if (!pdev) pdev = pci_find_device(0x1106, 0x3058);        /* VIA AC97 */
    if (!pdev) pdev = pci_find_class(0x04, 0x01);             /* Multimedia Audio Controller */

    if (!pdev) {
        serial_puts("[AC97] No AC97 audio controller found on PCI bus.\n");
        return -1;
    }

    serial_puts("[AC97] Found audio controller at PCI ");
    serial_puthex(pdev->bus);
    serial_puts(":");
    serial_puthex(pdev->slot);
    serial_puts(".");
    serial_puthex(pdev->func);
    serial_puts(" Vendor: ");
    serial_puthex(pdev->vendor_id);
    serial_puts(" Device: ");
    serial_puthex(pdev->device_id);
    serial_puts("\n");

    /* Enable bus mastering, I/O space and MMIO */
    pci_enable_bus_mastering(pdev);

    /* Get BAR0 (Mixer, NAMBAR) and BAR1 (Bus Master, NABMBAR) */
    ac97_nambar = (uint16_t)pdev->bars[0].base;
    ac97_nabmbar = (uint16_t)pdev->bars[1].base;
    ac97_irq = pdev->irq_line;

    if (!ac97_nambar || !ac97_nabmbar) {
        serial_puts("[AC97] Error: Invalid BAR0/BAR1 I/O ports.\n");
        return -1;
    }

    serial_puts("[AC97] NAMBAR (Mixer)=0x");
    serial_puthex(ac97_nambar);
    serial_puts(" NABMBAR (BusMaster)=0x");
    serial_puthex(ac97_nabmbar);
    serial_puts(" IRQ=");
    serial_puthex(ac97_irq);
    serial_puts("\n");

    /* Get HHDM offset for physical to virtual address translation */
    ac97_hhdm_offset = vmm_get_hhdm_offset();

    /* Step 1: Issue Cold Reset to AC'97 Controller */
    outl(ac97_nabmbar + AC97_GLOB_CNT, AC97_GCNT_COLD_RESET);
    ac97_delay_us(1000);

    /* Wait for Primary Codec Ready (PCR) in GLOB_STA */
    int timeout = 2000;
    while (!(inl(ac97_nabmbar + AC97_GLOB_STA) & AC97_GSTA_PCR)) {
        if (--timeout <= 0) {
            serial_puts("[AC97] Warning: Codec Ready timeout, continuing...\n");
            break;
        }
        ac97_delay_us(10);
    }

    /* Step 2: Reset Mixer & DAC Codec */
    ac97_mixer_write(AC97_RESET, 0);
    ac97_delay_us(1000);

    /* Step 3: Power up DAC and Codec components */
    ac97_mixer_write(AC97_POWERDOWN_CTRL, 0x0000); /* Clear any powerdown */
    ac97_delay_us(1000);

    /* Step 4: Reset PCM Out channel registers */
    outb(ac97_nabmbar + AC97_PO_CR, AC97_CR_RR);
    timeout = 1000;
    while (inb(ac97_nabmbar + AC97_PO_CR) & AC97_CR_RR) {
        if (--timeout <= 0) break;
        ac97_delay_us(10);
    }

    /* Step 5: Allocate Buffer Descriptor List (BDL) page */
    ac97_bdl_phys = (uint64_t)pmm_alloc_page();
    if (!ac97_bdl_phys) {
        serial_puts("[AC97] Error: Failed to allocate physical page for BDL.\n");
        return -1;
    }
    ac97_bdl = (struct ac97_bd *)(ac97_bdl_phys + ac97_hhdm_offset);

    /* Allocate sample buffers (32 buffers of 4096 bytes each = 32 pages) */
    for (int i = 0; i < AC97_NUM_BUFFERS; i++) {
        uint64_t phys = (uint64_t)pmm_alloc_page();
        if (!phys) {
            serial_puts("[AC97] Error: Failed to allocate audio sample buffer page.\n");
            return -1;
        }
        ac97_buf_phys[i] = phys;
        ac97_buffers[i] = (uint8_t *)(phys + ac97_hhdm_offset);

        /* Initialize buffer to silence */
        for (size_t b = 0; b < AC97_BUFFER_SIZE; b++) {
            ac97_buffers[i][b] = 0;
        }

        /* Fill descriptor */
        ac97_bdl[i].addr = (uint32_t)phys;
        ac97_bdl[i].length = (uint16_t)(AC97_BUFFER_SIZE / 2); /* 2048 16-bit samples */
        ac97_bdl[i].flags = AC97_BD_IOC;
    }

    /* Program PO_BDBAR with physical address of BDL */
    outl(ac97_nabmbar + AC97_PO_BDBAR, (uint32_t)ac97_bdl_phys);

    /* Reset LVI and CIV to 0 */
    outb(ac97_nabmbar + AC97_PO_LVI, 0);

    /* Step 6: Configure initial volume and sample rate */
    ac97_set_volume(100, 100);
    ac97_set_format(48000, 2, 16);

    /* Step 7: Register IRQ handler */
    uint8_t irq_vec = 0x20 + ac97_irq;
    register_interrupt_handler(irq_vec, ac97_irq_handler);

    if (apic_available()) {
        apic_irq_unmask(ac97_irq);
    } else if (ac97_irq < 8) {
        uint8_t mask = inb(0x21);
        mask &= ~(1 << ac97_irq);
        outb(0x21, mask);
    } else {
        uint8_t mask = inb(0xA1);
        mask &= ~(1 << (ac97_irq - 8));
        outb(0xA1, mask);
        uint8_t master_mask = inb(0x21);
        master_mask &= ~(1 << 2);
        outb(0x21, master_mask);
    }

    ac97_present = 1;
    ac97_head = 0;
    ac97_tail = 0;
    ac97_running = 0;

    /* Register with Sound Core */
    sound_register_card(&ac97_sound_card);

    serial_puts("[AC97] DAC97 audio driver initialized (48kHz 16-bit stereo, 128KB DMA ring).\n");
    return 0;
}
