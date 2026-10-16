/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * ac97.h - Intel 82801AA (ICH) AC'97 Audio Controller driver definitions
 */

#ifndef EIGEN_AC97_H
#define EIGEN_AC97_H

#include <stdint.h>
#include <stddef.h>

/* ── AC'97 Mixer Registers (NAMBAR, BAR0) ────────────────────────────────── */
#define AC97_RESET              0x00
#define AC97_MASTER_VOL         0x02
#define AC97_HEADPHONE_VOL      0x04
#define AC97_MONO_VOL           0x06
#define AC97_MASTER_TONE        0x08
#define AC97_PCBEEP_VOL         0x0A
#define AC97_PHONE_VOL          0x0C
#define AC97_MIC_VOL            0x0E
#define AC97_LINE_IN_VOL        0x10
#define AC97_CD_VOL             0x12
#define AC97_VIDEO_VOL          0x14
#define AC97_AUX_VOL            0x16
#define AC97_PCM_OUT_VOL        0x18
#define AC97_RECORD_SELECT      0x1A
#define AC97_RECORD_GAIN        0x1C
#define AC97_GENERAL_PURPOSE    0x20
#define AC97_3D_CONTROL         0x22
#define AC97_POWERDOWN_CTRL     0x26
#define AC97_EXTENDED_ID        0x28
#define AC97_EXTENDED_STAT      0x2A
#define AC97_FRONT_DAC_RATE     0x2C
#define AC97_SURR_DAC_RATE      0x2E
#define AC97_LFE_DAC_RATE       0x30
#define AC97_LR_DAC_RATE        0x32

/* ── AC'97 Bus Master Registers (NABMBAR, BAR1) ──────────────────────────── */
#define AC97_PO_BDBAR           0x10    /* Buffer Descriptor List Base Address (32-bit) */
#define AC97_PO_CIV             0x14    /* Current Index Value (8-bit) */
#define AC97_PO_LVI             0x15    /* Last Valid Index (8-bit) */
#define AC97_PO_SR              0x16    /* Status Register (16-bit) */
#define AC97_PO_PICB            0x18    /* Position in Current Buffer (16-bit) */
#define AC97_PO_PIV             0x1A    /* Prefetched Index Value (8-bit) */
#define AC97_PO_CR              0x1B    /* Control Register (8-bit) */
#define AC97_GLOB_CNT           0x2C    /* Global Control (32-bit) */
#define AC97_GLOB_STA           0x30    /* Global Status (32-bit) */

/* ── PO_SR status bits ───────────────────────────────────────────────────── */
#define AC97_SR_DCH             (1 << 0) /* DMA Controller Halted */
#define AC97_SR_CELV            (1 << 1) /* Current Equals Last Valid */
#define AC97_SR_LVBCI           (1 << 2) /* Last Valid Buffer Completion Interrupt */
#define AC97_SR_BCIS            (1 << 3) /* Buffer Completion Interrupt Status */
#define AC97_SR_FIFOE           (1 << 4) /* FIFO Error */

/* ── PO_CR control bits ──────────────────────────────────────────────────── */
#define AC97_CR_RPBM            (1 << 0) /* Run/Pause Bus Master (1=run, 0=pause) */
#define AC97_CR_RR              (1 << 1) /* Reset Registers */
#define AC97_CR_LVBIE           (1 << 2) /* Last Valid Buffer Interrupt Enable */
#define AC97_CR_FEIE            (1 << 3) /* FIFO Error Interrupt Enable */
#define AC97_CR_IOCE            (1 << 4) /* Interrupt On Completion Enable */

/* ── GLOB_CNT bits ───────────────────────────────────────────────────────── */
#define AC97_GCNT_COLD_RESET    (1 << 1)
#define AC97_GCNT_WARM_RESET    (1 << 2)

/* ── GLOB_STA bits ───────────────────────────────────────────────────────── */
#define AC97_GSTA_PCR           (1 << 8) /* Primary Codec Ready */

/* ── Buffer Descriptor Entry ─────────────────────────────────────────────── */
#define AC97_BD_IOC             (1 << 15) /* Interrupt On Completion */
#define AC97_BD_BUP             (1 << 14) /* Buffer Underrun Policy */

struct ac97_bd {
    uint32_t addr;      /* Physical memory address of buffer (32-bit) */
    uint16_t length;    /* Number of 16-bit samples (buffer byte length / 2) */
    uint16_t flags;     /* Bit 15: IOC, Bit 14: BUP */
} __attribute__((packed));

#define AC97_NUM_BUFFERS        32
#define AC97_BUFFER_SIZE        4096 /* bytes per buffer (2048 16-bit stereo samples) */

int ac97_init(void);

#endif /* EIGEN_AC97_H */
