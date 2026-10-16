/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * sound.c - Sound Core Subsystem (OSS /dev/dsp, /dev/mixer, and ALSA /dev/snd/)
 */

#include <stdint.h>
#include <stddef.h>
#include "sound.h"
#include "ac97.h"
#include "fs/vfs/vfs.h"
#include "drivers/timer/pit.h"
#include "include/serial.h"

/* ── Active Sound Card & Global Audio State ──────────────────────────────── */
static struct sound_card *g_active_card = NULL;

static uint32_t g_sample_rate = 48000;
static uint8_t  g_channels = 2;
static uint8_t  g_bits = 16;
static int      g_oss_format = AFMT_S16_LE;

static uint8_t  g_vol_left = 100;
static uint8_t  g_vol_right = 100;

static int      g_pcm_state = SNDRV_PCM_STATE_PREPARED;
static int      g_sound_initialized = 0;

/* ── String / Memory Helpers ─────────────────────────────────────────────── */
static void snd_strcpy(char *dst, const char *src, size_t max) {
    size_t i = 0;
    while (src[i] && i + 1 < max) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static int snd_strcmp(const char *s1, const char *s2) {
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(const unsigned char *)s1 - *(const unsigned char *)s2;
}

static void snd_memset(void *dst, int val, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    for (size_t i = 0; i < n; i++) d[i] = (uint8_t)val;
}

/* ── Registration API ────────────────────────────────────────────────────── */
int sound_register_card(struct sound_card *card) {
    if (!card) return -1;
    g_active_card = card;
    serial_puts("[SOUND] Registered card: ");
    serial_puts(card->long_name ? card->long_name : card->name);
    serial_puts("\n");

    /* Apply current volume & format to hardware */
    if (card->set_volume) card->set_volume(g_vol_left, g_vol_right);
    if (card->set_format) card->set_format(g_sample_rate, g_channels, g_bits);
    return 0;
}

struct sound_card *sound_get_active_card(void) {
    return g_active_card;
}

/* ── OSS /dev/dsp Implementation ─────────────────────────────────────────── */
static int dsp_open(struct vfs_node *node, int flags) {
    (void)node; (void)flags;
    g_pcm_state = SNDRV_PCM_STATE_PREPARED;
    return 0;
}

static int dsp_close(struct vfs_node *node) {
    (void)node;
    if (g_active_card && g_active_card->drain) {
        g_active_card->drain();
    }
    g_pcm_state = SNDRV_PCM_STATE_SETUP;
    return 0;
}

static int64_t dsp_read(struct vfs_node *node, uint64_t offset, size_t size, void *buf) {
    (void)node; (void)offset; (void)size; (void)buf;
    /* Audio capture not yet attached; return 0 for EOF */
    return 0;
}

static int64_t dsp_write(struct vfs_node *node, uint64_t offset, size_t size, const void *buf) {
    (void)node; (void)offset;
    if (!g_active_card || !g_active_card->write_pcm) return -19; /* -ENODEV */
    if (!buf || size == 0) return 0;

    g_pcm_state = SNDRV_PCM_STATE_RUNNING;
    return g_active_card->write_pcm(buf, size);
}

static int dsp_poll(struct vfs_node *node, int events) {
    (void)node;
    if (!g_active_card) return 0;
    int revents = 0;
    if (events & POLLOUT) {
        if (g_active_card->get_free_space && g_active_card->get_free_space() > 0) {
            revents |= POLLOUT;
        }
    }
    return revents & events;
}

static int dsp_ioctl(struct vfs_node *node, unsigned long cmd, void *arg) {
    (void)node;
    switch (cmd) {
        case SNDCTL_DSP_RESET:
            if (g_active_card && g_active_card->stop) g_active_card->stop();
            g_pcm_state = SNDRV_PCM_STATE_SETUP;
            return 0;

        case SNDCTL_DSP_SYNC:
            if (g_active_card && g_active_card->drain) g_active_card->drain();
            return 0;

        case SNDCTL_DSP_POST:
            if (g_active_card && g_active_card->start) g_active_card->start();
            return 0;

        case SNDCTL_DSP_SPEED: {
            if (!arg) return -14; /* -EFAULT */
            uint32_t val = *(uint32_t *)arg;
            if (val > 0) {
                g_sample_rate = val;
                if (g_active_card && g_active_card->set_format)
                    g_active_card->set_format(g_sample_rate, g_channels, g_bits);
            }
            *(uint32_t *)arg = g_sample_rate;
            return 0;
        }

        case SOUND_PCM_READ_RATE:
            if (!arg) return -14;
            *(uint32_t *)arg = g_sample_rate;
            return 0;

        case SNDCTL_DSP_STEREO: {
            if (!arg) return -14;
            int stereo = *(int *)arg;
            g_channels = stereo ? 2 : 1;
            if (g_active_card && g_active_card->set_format)
                g_active_card->set_format(g_sample_rate, g_channels, g_bits);
            *(int *)arg = (g_channels == 2) ? 1 : 0;
            return 0;
        }

        case SNDCTL_DSP_CHANNELS: {
            if (!arg) return -14;
            int ch = *(int *)arg;
            if (ch == 1 || ch == 2) {
                g_channels = (uint8_t)ch;
                if (g_active_card && g_active_card->set_format)
                    g_active_card->set_format(g_sample_rate, g_channels, g_bits);
            }
            *(int *)arg = g_channels;
            return 0;
        }

        case SOUND_PCM_READ_CHANNELS:
            if (!arg) return -14;
            *(int *)arg = g_channels;
            return 0;

        case SNDCTL_DSP_SETFMT: {
            if (!arg) return -14;
            int fmt = *(int *)arg;
            if (fmt == AFMT_U8) {
                g_oss_format = AFMT_U8;
                g_bits = 8;
            } else if (fmt == AFMT_S16_LE) {
                g_oss_format = AFMT_S16_LE;
                g_bits = 16;
            }
            if (g_active_card && g_active_card->set_format)
                g_active_card->set_format(g_sample_rate, g_channels, g_bits);
            *(int *)arg = g_oss_format;
            return 0;
        }

        case SOUND_PCM_READ_BITS:
            if (!arg) return -14;
            *(int *)arg = g_bits;
            return 0;

        case SNDCTL_DSP_GETFMTS:
            if (!arg) return -14;
            *(int *)arg = AFMT_S16_LE | AFMT_U8;
            return 0;

        case SNDCTL_DSP_GETBLKSIZE:
            if (!arg) return -14;
            *(int *)arg = 4096;
            return 0;

        case SNDCTL_DSP_GETOSPACE: {
            if (!arg) return -14;
            struct audio_buf_info *info = (struct audio_buf_info *)arg;
            size_t free_bytes = g_active_card && g_active_card->get_free_space ?
                                g_active_card->get_free_space() : 65536;
            info->fragsize = 4096;
            info->fragstotal = 32;
            info->fragments = (int)(free_bytes / 4096);
            info->bytes = (int)free_bytes;
            return 0;
        }

        case SNDCTL_DSP_NONBLOCK:
            return 0;

        case SNDCTL_DSP_GETCAPS:
            if (!arg) return -14;
            *(int *)arg = 0x00000001; /* DSP_CAP_DUPLEX */
            return 0;

        default:
            return -25; /* -ENOTTY */
    }
}

/* ── OSS /dev/mixer Implementation ───────────────────────────────────────── */
static int mixer_ioctl(struct vfs_node *node, unsigned long cmd, void *arg) {
    (void)node;
    switch (cmd) {
        case SOUND_MIXER_READ_VOLUME:
        case SOUND_MIXER_READ_PCM:
            if (!arg) return -14;
            *(int *)arg = (g_vol_left) | (g_vol_right << 8);
            return 0;

        case SOUND_MIXER_WRITE_VOLUME:
        case SOUND_MIXER_WRITE_PCM: {
            if (!arg) return -14;
            int val = *(int *)arg;
            uint8_t l = (uint8_t)(val & 0xFF);
            uint8_t r = (uint8_t)((val >> 8) & 0xFF);
            if (l > 100) l = 100;
            if (r > 100) r = 100;
            g_vol_left = l;
            g_vol_right = r;
            if (g_active_card && g_active_card->set_volume)
                g_active_card->set_volume(l, r);
            *(int *)arg = (g_vol_left) | (g_vol_right << 8);
            return 0;
        }

        case SOUND_MIXER_READ_DEVMASK:
            if (!arg) return -14;
            *(int *)arg = SOUND_MASK_VOLUME | SOUND_MASK_PCM;
            return 0;

        case SOUND_MIXER_READ_RECMASK:
            if (!arg) return -14;
            *(int *)arg = 0;
            return 0;

        case SOUND_MIXER_READ_STEREODEVS:
            if (!arg) return -14;
            *(int *)arg = SOUND_MASK_VOLUME | SOUND_MASK_PCM;
            return 0;

        case SOUND_MIXER_READ_CAPS:
            if (!arg) return -14;
            *(int *)arg = 0;
            return 0;

        default:
            return -25; /* -ENOTTY */
    }
}

/* ── ALSA /dev/snd/controlC0 Implementation ──────────────────────────────── */
static int alsa_control_ioctl(struct vfs_node *node, unsigned long cmd, void *arg) {
    (void)node;
    switch (cmd) {
        case SNDRV_CTL_IOCTL_PVERSION:
            if (!arg) return -14;
            *(int *)arg = 0x00020007; /* 2.0.7 */
            return 0;

        case SNDRV_CTL_IOCTL_CARD_INFO: {
            if (!arg) return -14;
            struct snd_ctl_card_info *info = (struct snd_ctl_card_info *)arg;
            snd_memset(info, 0, sizeof(*info));
            info->card = 0;
            snd_strcpy((char *)info->id, "AC97", sizeof(info->id));
            snd_strcpy((char *)info->driver, "IntelICH", sizeof(info->driver));
            snd_strcpy((char *)info->name, "AC97 Audio", sizeof(info->name));
            snd_strcpy((char *)info->longname, "Intel ICH AC97 / DAC97 Audio", sizeof(info->longname));
            snd_strcpy((char *)info->mixername, "AC97 Mixer", sizeof(info->mixername));
            return 0;
        }

        default:
            return -25;
    }
}

/* ── ALSA /dev/snd/pcmC0D0p Implementation ───────────────────────────────── */
static int alsa_pcm_ioctl(struct vfs_node *node, unsigned long cmd, void *arg) {
    (void)node;
    switch (cmd) {
        case SNDRV_PCM_IOCTL_PVERSION:
            if (!arg) return -14;
            *(int *)arg = 0x00020007; /* 2.0.7 */
            return 0;

        case SNDRV_PCM_IOCTL_INFO: {
            if (!arg) return -14;
            struct snd_pcm_info *info = (struct snd_pcm_info *)arg;
            snd_memset(info, 0, sizeof(*info));
            info->device = 0;
            info->subdevice = 0;
            info->stream = 0; /* Playback */
            info->card = 0;
            snd_strcpy((char *)info->id, "AC97 PCM", sizeof(info->id));
            snd_strcpy((char *)info->name, "AC97 PCM", sizeof(info->name));
            info->subdevices_count = 1;
            info->subdevices_avail = 1;
            return 0;
        }

        case SNDRV_PCM_IOCTL_HW_REFINE:
        case SNDRV_PCM_IOCTL_HW_PARAMS: {
            if (!arg) return -14;
            struct snd_pcm_hw_params *hw = (struct snd_pcm_hw_params *)arg;
            /* Configure hardware matching client requests */
            g_sample_rate = 48000;
            g_channels = 2;
            g_bits = 16;
            if (g_active_card && g_active_card->set_format)
                g_active_card->set_format(g_sample_rate, g_channels, g_bits);

            /* Fill intervals with sane values: rate=48000, channels=2 */
            hw->intervals[2].min = 2; /* CHANNELS */
            hw->intervals[2].max = 2;
            hw->intervals[3].min = g_sample_rate; /* RATE */
            hw->intervals[3].max = g_sample_rate;
            hw->intervals[5].min = 1024; /* PERIOD_SIZE */
            hw->intervals[5].max = 1024;
            hw->intervals[7].min = 4;    /* PERIODS */
            hw->intervals[7].max = 4;
            hw->intervals[9].min = 4096; /* BUFFER_SIZE */
            hw->intervals[9].max = 4096;
            g_pcm_state = SNDRV_PCM_STATE_PREPARED;
            return 0;
        }

        case SNDRV_PCM_IOCTL_SW_PARAMS:
            return 0;

        case SNDRV_PCM_IOCTL_PREPARE:
            g_pcm_state = SNDRV_PCM_STATE_PREPARED;
            return 0;

        case SNDRV_PCM_IOCTL_START:
            g_pcm_state = SNDRV_PCM_STATE_RUNNING;
            if (g_active_card && g_active_card->start) g_active_card->start();
            return 0;

        case SNDRV_PCM_IOCTL_DROP:
            if (g_active_card && g_active_card->stop) g_active_card->stop();
            g_pcm_state = SNDRV_PCM_STATE_SETUP;
            return 0;

        case SNDRV_PCM_IOCTL_DRAIN:
            if (g_active_card && g_active_card->drain) g_active_card->drain();
            g_pcm_state = SNDRV_PCM_STATE_SETUP;
            return 0;

        case SNDRV_PCM_IOCTL_STATUS: {
            if (!arg) return -14;
            struct snd_pcm_status *st = (struct snd_pcm_status *)arg;
            snd_memset(st, 0, sizeof(*st));
            st->state = g_pcm_state;
            st->avail = g_active_card && g_active_card->get_free_space ?
                        (g_active_card->get_free_space() / 4) : 1024;
            return 0;
        }

        case SNDRV_PCM_IOCTL_DELAY:
            if (!arg) return -14;
            *(int64_t *)arg = 0;
            return 0;

        case SNDRV_PCM_IOCTL_WRITEI_FRAMES: {
            if (!arg) return -14;
            struct snd_xferi *xferi = (struct snd_xferi *)arg;
            if (!xferi->buf || xferi->frames == 0) {
                xferi->result = 0;
                return 0;
            }
            if (!g_active_card || !g_active_card->write_pcm) {
                xferi->result = -19;
                return -19;
            }

            size_t frame_bytes = (size_t)(g_channels * (g_bits / 8));
            size_t total_bytes = (size_t)xferi->frames * frame_bytes;

            g_pcm_state = SNDRV_PCM_STATE_RUNNING;
            int64_t written = g_active_card->write_pcm(xferi->buf, total_bytes);
            if (written > 0) {
                xferi->result = (int64_t)(written / frame_bytes);
            } else {
                xferi->result = written;
            }
            return 0;
        }

        default:
            /* Fallback to OSS handler for common ioctls */
            return dsp_ioctl(node, cmd, arg);
    }
}

/* ── VFS Device Nodes ────────────────────────────────────────────────────── */
static struct vfs_node g_dsp_node;
static struct vfs_node g_mixer_node;
static struct vfs_node g_snd_dir;
static struct vfs_node g_snd_control_node;
static struct vfs_node g_snd_pcm_node;
static struct vfs_node g_snd_pcm_cap_node;
static struct vfs_node g_snd_timer_node;

/* ── Directory Lookup for /dev/snd/ ──────────────────────────────────────── */
static struct vfs_node *snd_finddir(struct vfs_node *node, const char *name) {
    (void)node;
    if (snd_strcmp(name, "controlC0") == 0) return &g_snd_control_node;
    if (snd_strcmp(name, "pcmC0D0p") == 0)  return &g_snd_pcm_node;
    if (snd_strcmp(name, "pcmC0D0c") == 0)  return &g_snd_pcm_cap_node;
    if (snd_strcmp(name, "timer") == 0)     return &g_snd_timer_node;
    return NULL;
}

static struct vfs_dirent *snd_readdir(struct vfs_node *node, uint32_t index) {
    (void)node;
    static struct vfs_dirent dent;
    const char *names[4] = { "controlC0", "pcmC0D0p", "pcmC0D0c", "timer" };
    uint64_t inos[4] = { 71, 72, 73, 74 };

    if (index >= 4) return NULL;

    snd_strcpy(dent.name, names[index], sizeof(dent.name));
    dent.ino = inos[index];
    dent.type = VFS_CHARDEVICE;
    dent.size = 0;
    return &dent;
}

/* ── Sound Subsystem Initialization ──────────────────────────────────────── */
void sound_init(void) {
    if (g_sound_initialized) return;

    /* Initialize /dev/dsp */
    snd_strcpy(g_dsp_node.name, "dsp", sizeof(g_dsp_node.name));
    g_dsp_node.type = VFS_CHARDEVICE;
    g_dsp_node.permissions = 0666;
    g_dsp_node.inode = 65;
    g_dsp_node.read = dsp_read;
    g_dsp_node.write = dsp_write;
    g_dsp_node.open = dsp_open;
    g_dsp_node.close = dsp_close;
    g_dsp_node.ioctl = dsp_ioctl;
    g_dsp_node.poll = dsp_poll;

    /* Initialize /dev/mixer */
    snd_strcpy(g_mixer_node.name, "mixer", sizeof(g_mixer_node.name));
    g_mixer_node.type = VFS_CHARDEVICE;
    g_mixer_node.permissions = 0666;
    g_mixer_node.inode = 66;
    g_mixer_node.ioctl = mixer_ioctl;

    /* Initialize /dev/snd directory */
    snd_strcpy(g_snd_dir.name, "snd", sizeof(g_snd_dir.name));
    g_snd_dir.type = VFS_DIRECTORY;
    g_snd_dir.permissions = 0755;
    g_snd_dir.inode = 70;
    g_snd_dir.finddir = snd_finddir;
    g_snd_dir.readdir = snd_readdir;

    /* Initialize /dev/snd/controlC0 */
    snd_strcpy(g_snd_control_node.name, "controlC0", sizeof(g_snd_control_node.name));
    g_snd_control_node.type = VFS_CHARDEVICE;
    g_snd_control_node.permissions = 0666;
    g_snd_control_node.inode = 71;
    g_snd_control_node.ioctl = alsa_control_ioctl;

    /* Initialize /dev/snd/pcmC0D0p */
    snd_strcpy(g_snd_pcm_node.name, "pcmC0D0p", sizeof(g_snd_pcm_node.name));
    g_snd_pcm_node.type = VFS_CHARDEVICE;
    g_snd_pcm_node.permissions = 0666;
    g_snd_pcm_node.inode = 72;
    g_snd_pcm_node.read = dsp_read;
    g_snd_pcm_node.write = dsp_write;
    g_snd_pcm_node.open = dsp_open;
    g_snd_pcm_node.close = dsp_close;
    g_snd_pcm_node.ioctl = alsa_pcm_ioctl;
    g_snd_pcm_node.poll = dsp_poll;

    /* Initialize /dev/snd/pcmC0D0c (capture stub) */
    snd_strcpy(g_snd_pcm_cap_node.name, "pcmC0D0c", sizeof(g_snd_pcm_cap_node.name));
    g_snd_pcm_cap_node.type = VFS_CHARDEVICE;
    g_snd_pcm_cap_node.permissions = 0666;
    g_snd_pcm_cap_node.inode = 73;

    /* Initialize /dev/snd/timer */
    snd_strcpy(g_snd_timer_node.name, "timer", sizeof(g_snd_timer_node.name));
    g_snd_timer_node.type = VFS_CHARDEVICE;
    g_snd_timer_node.permissions = 0666;
    g_snd_timer_node.inode = 74;

    /* Attach device nodes to devfs */
    devfs_attach(&g_dsp_node);
    devfs_attach(&g_mixer_node);
    devfs_attach(&g_snd_dir);

    /* Probe and initialize AC'97 hardware controller */
    ac97_init();

    g_sound_initialized = 1;
    serial_puts("[SOUND] Sound subsystem ready (/dev/dsp, /dev/mixer, /dev/snd/)\n");
}
