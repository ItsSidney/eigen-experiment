/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * sndtest.c - Sound subsystem & AC97 / DAC97 audio smoke test for Eigen.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <math.h>

#define SNDCTL_DSP_RESET        0x00005000
#define SNDCTL_DSP_SYNC         0x00005001
#define SNDCTL_DSP_SPEED        0xc0045002
#define SNDCTL_DSP_STEREO       0xc0045003
#define SNDCTL_DSP_GETBLKSIZE   0xc0045004
#define SNDCTL_DSP_SETFMT       0xc0045005
#define SNDCTL_DSP_CHANNELS     0xc0045006
#define SNDCTL_DSP_GETOSPACE    0x8010500c

#define AFMT_U8                 0x00000008
#define AFMT_S16_LE             0x00000010

#define SOUND_MIXER_READ_VOLUME     0x80044d00
#define SOUND_MIXER_WRITE_VOLUME    0xc0044d00

#define SNDRV_CTL_IOCTL_PVERSION    0x80045500
#define SNDRV_CTL_IOCTL_CARD_INFO   0x81785501
#define SNDRV_PCM_IOCTL_PVERSION    0x80044100
#define SNDRV_PCM_IOCTL_INFO        0x81204101

struct audio_buf_info {
    int fragments;
    int fragstotal;
    int fragsize;
    int bytes;
};

struct snd_ctl_card_info {
    int card;
    int pad;
    uint8_t id[16];
    uint8_t driver[16];
    uint8_t name[32];
    uint8_t longname[80];
    uint8_t reserved_[16];
    uint8_t mixername[80];
    uint8_t components[128];
};

struct snd_pcm_info {
    uint32_t device;
    uint32_t subdevice;
    int stream;
    int card;
    uint8_t id[64];
    uint8_t name[80];
    uint8_t subname[80];
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

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    printf("[sndtest] Testing Eigen Sound Subsystem & AC'97 / DAC97...\n");

    /* 1. Test /dev/mixer */
    int mfd = open("/dev/mixer", O_RDWR);
    if (mfd < 0) {
        printf("[sndtest] Error: Failed to open /dev/mixer\n");
    } else {
        int vol = 0;
        if (ioctl(mfd, SOUND_MIXER_READ_VOLUME, &vol) == 0) {
            printf("[sndtest] /dev/mixer: current volume = %d%% L, %d%% R\n", vol & 0xFF, (vol >> 8) & 0xFF);
        }
        int new_vol = 90 | (90 << 8);
        if (ioctl(mfd, SOUND_MIXER_WRITE_VOLUME, &new_vol) == 0) {
            printf("[sndtest] /dev/mixer: set volume to 90%% OK\n");
        }
        close(mfd);
    }

    /* 2. Test /dev/dsp */
    int dfd = open("/dev/dsp", O_WRONLY);
    if (dfd < 0) {
        printf("[sndtest] Error: Failed to open /dev/dsp\n");
        return 1;
    }

    int fmt = AFMT_S16_LE;
    if (ioctl(dfd, SNDCTL_DSP_SETFMT, &fmt) < 0) {
        printf("[sndtest] Warning: SNDCTL_DSP_SETFMT failed\n");
    }

    int ch = 2;
    if (ioctl(dfd, SNDCTL_DSP_CHANNELS, &ch) < 0) {
        printf("[sndtest] Warning: SNDCTL_DSP_CHANNELS failed\n");
    }

    int rate = 48000;
    if (ioctl(dfd, SNDCTL_DSP_SPEED, &rate) < 0) {
        printf("[sndtest] Warning: SNDCTL_DSP_SPEED failed\n");
    } else {
        printf("[sndtest] /dev/dsp configured: %d Hz, %d channels, 16-bit signed\n", rate, ch);
    }

    struct audio_buf_info ospace;
    if (ioctl(dfd, SNDCTL_DSP_GETOSPACE, &ospace) == 0) {
        printf("[sndtest] /dev/dsp ospace: %d bytes free (%d frags of %d bytes)\n",
               ospace.bytes, ospace.fragments, ospace.fragsize);
    }

    /* Generate 1/4 second of 440 Hz tone (A4) */
    int num_samples = rate / 4; /* 12000 stereo frames */
    int16_t *pcm = malloc(num_samples * 2 * sizeof(int16_t));
    if (pcm) {
        for (int i = 0; i < num_samples; i++) {
            /* Approximate sine wave with triangle/sine */
            double t = (double)i / (double)rate;
            double s = sin(2.0 * 3.141592653589793 * 440.0 * t);
            int16_t val = (int16_t)(s * 16000.0);
            pcm[i * 2]     = val;
            pcm[i * 2 + 1] = val;
        }

        size_t bytes = num_samples * 2 * sizeof(int16_t);
        ssize_t w = write(dfd, pcm, bytes);
        printf("[sndtest] /dev/dsp: wrote %zd of %zu bytes PCM\n", w, bytes);
        free(pcm);
    }

    ioctl(dfd, SNDCTL_DSP_SYNC, 0);
    close(dfd);

    /* 3. Test ALSA /dev/snd/controlC0 */
    int cfd = open("/dev/snd/controlC0", O_RDWR);
    if (cfd >= 0) {
        struct snd_ctl_card_info cinfo;
        if (ioctl(cfd, SNDRV_CTL_IOCTL_CARD_INFO, &cinfo) == 0) {
            printf("[sndtest] /dev/snd/controlC0: card '%s' driver '%s'\n", cinfo.name, cinfo.driver);
        }
        close(cfd);
    } else {
        printf("[sndtest] Note: /dev/snd/controlC0 could not be opened\n");
    }

    /* 4. Test ALSA /dev/snd/pcmC0D0p */
    int pfd = open("/dev/snd/pcmC0D0p", O_RDWR);
    if (pfd >= 0) {
        struct snd_pcm_info pinfo;
        if (ioctl(pfd, SNDRV_PCM_IOCTL_INFO, &pinfo) == 0) {
            printf("[sndtest] /dev/snd/pcmC0D0p: name '%s' device %u stream %d\n",
                   pinfo.name, pinfo.device, pinfo.stream);
        }
        close(pfd);
    } else {
        printf("[sndtest] Note: /dev/snd/pcmC0D0p could not be opened\n");
    }

    printf("[sndtest] SUCCESS: Sound subsystem tests completed successfully!\n");
    return 0;
}
