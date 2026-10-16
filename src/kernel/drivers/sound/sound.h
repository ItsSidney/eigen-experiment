/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * sound.h - Eigen Sound Subsystem (OSS /dev/dsp and ALSA /dev/snd/ compatibility)
 */

#ifndef EIGEN_SOUND_H
#define EIGEN_SOUND_H

#include <stdint.h>
#include <stddef.h>
#include "fs/vfs/vfs.h"

/* ── OSS Sample Formats ──────────────────────────────────────────────────── */
#define AFMT_QUERY          0x00000000
#define AFMT_MU_LAW         0x00000001
#define AFMT_A_LAW          0x00000002
#define AFMT_IMA_ADPCM      0x00000004
#define AFMT_U8             0x00000008
#define AFMT_S16_LE         0x00000010
#define AFMT_S16_BE         0x00000020
#define AFMT_S8             0x00000040
#define AFMT_U16_LE         0x00000080
#define AFMT_U16_BE         0x00000100

/* ── OSS DSP ioctl commands ──────────────────────────────────────────────── */
#define SNDCTL_DSP_RESET        0x00005000
#define SNDCTL_DSP_SYNC         0x00005001
#define SNDCTL_DSP_SPEED        0xc0045002
#define SNDCTL_DSP_STEREO       0xc0045003
#define SNDCTL_DSP_GETBLKSIZE   0xc0045004
#define SNDCTL_DSP_SETFMT       0xc0045005
#define SNDCTL_DSP_CHANNELS     0xc0045006
#define SNDCTL_DSP_POST         0x00005008
#define SNDCTL_DSP_SUBDIVIDE    0xc0045009
#define SNDCTL_DSP_SETFRAGMENT  0xc004500a
#define SNDCTL_DSP_GETFMTS      0x8004500b
#define SNDCTL_DSP_GETOSPACE    0x8010500c
#define SNDCTL_DSP_GETISPACE    0x8010500d
#define SNDCTL_DSP_NONBLOCK     0x0000500e
#define SNDCTL_DSP_GETCAPS      0x8004500f

#define SOUND_PCM_READ_RATE     0x80045002
#define SOUND_PCM_READ_CHANNELS 0x80045006
#define SOUND_PCM_READ_BITS     0x80045005

/* ── OSS Mixer ioctl commands ────────────────────────────────────────────── */
#define SOUND_MIXER_READ_VOLUME     0x80044d00
#define SOUND_MIXER_WRITE_VOLUME    0xc0044d00
#define SOUND_MIXER_READ_BASS       0x80044d01
#define SOUND_MIXER_WRITE_BASS      0xc0044d01
#define SOUND_MIXER_READ_TREBLE     0x80044d02
#define SOUND_MIXER_WRITE_TREBLE    0xc0044d02
#define SOUND_MIXER_READ_SYNTH      0x80044d03
#define SOUND_MIXER_WRITE_SYNTH     0xc0044d03
#define SOUND_MIXER_READ_PCM        0x80044d04
#define SOUND_MIXER_WRITE_PCM       0xc0044d04
#define SOUND_MIXER_READ_SPEAKER    0x80044d05
#define SOUND_MIXER_WRITE_SPEAKER   0xc0044d05
#define SOUND_MIXER_READ_LINE       0x80044d06
#define SOUND_MIXER_WRITE_LINE      0xc0044d06
#define SOUND_MIXER_READ_MIC        0x80044d07
#define SOUND_MIXER_WRITE_MIC       0xc0044d07
#define SOUND_MIXER_READ_DEVMASK    0x80044dff
#define SOUND_MIXER_READ_RECMASK    0x80044dfe
#define SOUND_MIXER_READ_RECSRC     0x80044dfd
#define SOUND_MIXER_WRITE_RECSRC    0xc0044dfd
#define SOUND_MIXER_READ_STEREODEVS 0x80044dfb
#define SOUND_MIXER_READ_CAPS       0x80044dfc

#define SOUND_MASK_VOLUME   (1 << 0)
#define SOUND_MASK_PCM      (1 << 4)
#define SOUND_MASK_SPEAKER  (1 << 5)

struct audio_buf_info {
    int fragments;  /* # of available fragments */
    int fragstotal; /* total # of fragments allocated */
    int fragsize;   /* size of a fragment in bytes */
    int bytes;      /* available space in bytes (fragments * fragsize) */
};

/* ── ALSA ioctl commands ─────────────────────────────────────────────────── */
#define SNDRV_CTL_IOCTL_PVERSION        0x80045500
#define SNDRV_CTL_IOCTL_CARD_INFO       0x81785501

#define SNDRV_PCM_IOCTL_PVERSION        0x80044100
#define SNDRV_PCM_IOCTL_INFO            0x81204101
#define SNDRV_PCM_IOCTL_TSTAMP          0x40044102
#define SNDRV_PCM_IOCTL_TTSTAMP         0x40044103
#define SNDRV_PCM_IOCTL_HW_REFINE       0xc2604110
#define SNDRV_PCM_IOCTL_HW_PARAMS       0xc2604111
#define SNDRV_PCM_IOCTL_HW_FREE         0x20004112
#define SNDRV_PCM_IOCTL_SW_PARAMS       0xc0884113
#define SNDRV_PCM_IOCTL_STATUS          0x80984120
#define SNDRV_PCM_IOCTL_DELAY           0x80084121
#define SNDRV_PCM_IOCTL_HWSYNC          0x20004122
#define SNDRV_PCM_IOCTL_SYNC_PTR        0xc0884123
#define SNDRV_PCM_IOCTL_CHANNEL_INFO    0x80184132
#define SNDRV_PCM_IOCTL_PREPARE         0x20004140
#define SNDRV_PCM_IOCTL_RESET           0x20004141
#define SNDRV_PCM_IOCTL_START           0x20004142
#define SNDRV_PCM_IOCTL_DROP            0x20004143
#define SNDRV_PCM_IOCTL_DRAIN           0x20004144
#define SNDRV_PCM_IOCTL_PAUSE           0x40044145
#define SNDRV_PCM_IOCTL_REWIND          0x40084146
#define SNDRV_PCM_IOCTL_RESUME          0x20004147
#define SNDRV_PCM_IOCTL_XRUN            0x20004148
#define SNDRV_PCM_IOCTL_FORWARD         0x40084149
#define SNDRV_PCM_IOCTL_WRITEI_FRAMES   0x40104150
#define SNDRV_PCM_IOCTL_READI_FRAMES    0x40104151

#define SNDRV_PCM_FORMAT_S8         0
#define SNDRV_PCM_FORMAT_U8         1
#define SNDRV_PCM_FORMAT_S16_LE     2
#define SNDRV_PCM_FORMAT_S16_BE     3
#define SNDRV_PCM_FORMAT_U16_LE     4
#define SNDRV_PCM_FORMAT_U16_BE     5

#define SNDRV_PCM_STATE_OPEN        0
#define SNDRV_PCM_STATE_SETUP       1
#define SNDRV_PCM_STATE_PREPARED    2
#define SNDRV_PCM_STATE_RUNNING     3
#define SNDRV_PCM_STATE_XRUN        4
#define SNDRV_PCM_STATE_DRAINING    5
#define SNDRV_PCM_STATE_PAUSED      6
#define SNDRV_PCM_STATE_SUSPENDED   7
#define SNDRV_PCM_STATE_DISCONNECTED 8

struct snd_ctl_card_info {
    int card;                   /* card index */
    int pad;
    uint8_t id[16];             /* ID name of card by user */
    uint8_t driver[16];         /* Driver name */
    uint8_t name[32];           /* Short name of soundcard */
    uint8_t longname[80];       /* Name + info text about soundcard */
    uint8_t reserved_[16];
    uint8_t mixername[80];      /* Mixer name */
    uint8_t components[128];    /* card components / identification */
};

struct snd_pcm_info {
    uint32_t device;            /* device number */
    uint32_t subdevice;         /* subdevice number */
    int stream;                 /* 0 = playback, 1 = capture */
    int card;                   /* card number */
    uint8_t id[64];             /* ID (user selectable) */
    uint8_t name[80];           /* name of this device */
    uint8_t subname[80];        /* subdevice name */
    int dev_class;
    int dev_subclass;
    uint32_t subdevices_count;
    uint32_t subdevices_avail;
    union {
        uint8_t id[16];
        uint32_t id32[4];
    } sync;
    uint8_t reserved[64];
};

struct snd_interval {
    uint32_t min, max;
    uint32_t openmin:1,
             openmax:1,
             integer:1,
             empty:1;
};

struct snd_mask {
    uint32_t bits[8];
};

struct snd_pcm_hw_params {
    uint32_t flags;
    struct snd_mask masks[3];     /* ACCESS, FORMAT, SUBFORMAT */
    struct snd_mask mres[5];      /* reserved masks */
    struct snd_interval intervals[12]; /* SAMPLE_BITS, FRAME_BITS, CHANNELS, RATE, PERIOD_TIME, PERIOD_SIZE, PERIOD_BYTES, PERIODS, BUFFER_TIME, BUFFER_SIZE, BUFFER_BYTES, TICK_TIME */
    struct snd_interval ires[9];
    uint32_t rmask;
    uint32_t cmask;
    uint32_t info;
    uint32_t msbits;
    uint32_t rate_num;
    uint32_t rate_den;
    uint64_t fifo_size;
    uint8_t  reserved[64];
};

struct snd_pcm_sw_params {
    int tstamp_mode;
    uint32_t period_step;
    uint32_t sleep_min;
    uint64_t avail_min;
    uint64_t xfer_align;
    uint64_t start_threshold;
    uint64_t stop_threshold;
    uint64_t silence_threshold;
    uint64_t silence_size;
    uint64_t boundary;
    uint32_t proto;
    uint32_t tstamp_type;
    uint8_t  reserved[56];
};

struct snd_xferi {
    int64_t result;
    void *buf;
    uint64_t frames;
};

struct snd_pcm_status {
    int state;
    uint64_t trigger_tstamp_sec;
    uint64_t trigger_tstamp_nsec;
    uint64_t tstamp_sec;
    uint64_t tstamp_nsec;
    int64_t  appl_ptr;
    int64_t  hw_ptr;
    int64_t  delay;
    uint64_t avail;
    uint64_t avail_max;
    uint64_t overrange;
    int suspended_state;
    uint32_t audio_tstamp_data;
    uint64_t audio_tstamp_sec;
    uint64_t audio_tstamp_nsec;
    uint64_t driver_tstamp_sec;
    uint64_t driver_tstamp_nsec;
    uint32_t audio_tstamp_accuracy;
    uint8_t  reserved[52];
};

/* ── Sound Card Interface ────────────────────────────────────────────────── */
struct sound_card {
    const char *name;
    const char *driver_name;
    const char *long_name;

    /* Hardware configuration */
    int (*set_format)(uint32_t sample_rate, uint8_t channels, uint8_t bits_per_sample);
    int (*set_volume)(uint8_t left, uint8_t right);
    int (*get_volume)(uint8_t *left, uint8_t *right);

    /* Playback streaming */
    int64_t (*write_pcm)(const void *buf, size_t count);
    size_t (*get_free_space)(void);
    int (*start)(void);
    int (*stop)(void);
    void (*drain)(void);
};

/* ── Sound Subsystem API ─────────────────────────────────────────────────── */
void sound_init(void);
int  sound_register_card(struct sound_card *card);
struct sound_card *sound_get_active_card(void);

#endif /* EIGEN_SOUND_H */
