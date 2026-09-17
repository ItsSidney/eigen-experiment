/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * shell.c - pty shell implementation. Userspace (musl/POSIX).
 */

#define _DEFAULT_SOURCE /* posix_openpt, ptsname */
#define _XOPEN_SOURCE 600

#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include "shell/shell.h"

int shell_spawn(struct shell_state *s) {
    int m;
    char *slave_name;
    pid_t pid;
    if (s->running)
        return 0;
    if (!s->buf) {
        s->buf = malloc(SHELL_CAP);
        s->subs = malloc(sizeof(struct lat_sub) * 4);
        if (!s->buf || !s->subs) {
            free(s->buf);
            free(s->subs);
            s->buf = NULL;
            s->subs = NULL;
            return -1;
        }
        lat_stream_init(&s->out, s->buf, SHELL_CAP, s->subs, 4);
    }
    m = posix_openpt(O_RDWR | O_NOCTTY);
    if (m < 0)
        return -1;
    if (grantpt(m) != 0 || unlockpt(m) != 0) {
        close(m);
        return -1;
    }
    slave_name = ptsname(m);
    if (!slave_name) {
        close(m);
        return -1;
    }
    {
        char namebuf[64];
        strncpy(namebuf, slave_name, sizeof namebuf - 1);
        namebuf[sizeof namebuf - 1] = '\0';
        pid = fork();
        if (pid < 0) {
            close(m);
            return -1;
        }
        if (pid == 0) {
            int sl;
            setsid();
            sl = open(namebuf, O_RDWR);
            if (sl < 0)
                _exit(127);
            ioctl(sl, TIOCSCTTY, 0);
            dup2(sl, 0);
            dup2(sl, 1);
            dup2(sl, 2);
            if (sl > 2)
                close(sl);
            close(m);
            signal(SIGINT, SIG_DFL);
            /* kernel-shell look, fully functional ash underneath */
            setenv("PS1", "eigen-kernel# ", 1);
            execl("/bin/sh", "sh", (char *)NULL);
            _exit(127);
        }
    }
    {
        int flags = fcntl(m, F_GETFL, 0);
        fcntl(m, F_SETFL, flags | O_NONBLOCK);
    }
    s->master = m;
    s->pid = pid;
    s->running = 1;
    {
        static const char banner[] = "[recovery shell on pty]\r\n";
        lat_stream_push(&s->out, banner, sizeof banner - 1);
    }
    return 0;
}

int shell_pump(struct shell_state *s) {
    static uint8_t chunk[1024];
    ssize_t n;
    int moved = 0;
    int status;
    pid_t w;
    if (!s->running || s->master < 0)
        return 0;
    for (;;) {
        n = read(s->master, chunk, sizeof chunk);
        if (n <= 0)
            break;
        lat_stream_push(&s->out, chunk, (size_t)n);
        moved = 1;
    }
    w = waitpid(s->pid, &status, WNOHANG);
    if (w == s->pid) {
        static const char dead[] = "\r\n[shell exited]\r\n";
        lat_stream_push(&s->out, dead, sizeof dead - 1);
        close(s->master);
        s->master = -1;
        s->running = 0;
        moved = 1;
    }
    return moved;
}

void shell_send(struct shell_state *s, char c) {
    if (!s->running || s->master < 0)
        return;
    (void)write(s->master, &c, 1);
}

void shell_free(struct shell_state *s) {
    if (s->running && s->pid > 0) {
        kill(s->pid, SIGHUP);
        waitpid(s->pid, NULL, 0);
    }
    if (s->master >= 0)
        close(s->master);
    free(s->buf);
    free(s->subs);
    s->buf = NULL;
    s->subs = NULL;
    s->master = -1;
    s->running = 0;
}
