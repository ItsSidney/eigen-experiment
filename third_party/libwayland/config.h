/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * config.h - feature probe results for the Eigen port of libwayland.
 *
 * Meson normally generates this. The values below reflect what this kernel
 * actually provides: epoll/accept4/memfd_create are implemented, the BSD and
 * Solaris paths are not (and are #ifdef'd out upstream), and pthreads exist
 * in musl but the kernel cannot create threads yet, so nothing here may
 * depend on a second thread making progress.
 */

#ifndef WAYLAND_CONFIG_H
#define WAYLAND_CONFIG_H

/* Kernel-provided primitives this port relies on. */
#define HAVE_ACCEPT4            1
#define HAVE_EPOLL_CREATE1      1
#define HAVE_MEMFD_CREATE       1
#define HAVE_PRCTL              1
#define HAVE_SYS_PRCTL_H        1
#define HAVE_MKOSTEMP           1
#define HAVE_POSIX_FALLOCATE    1

/* Deliberately NOT defined: upstream guards these with #ifdef, so defining
 * them to 0 would still enable the BSD/Solaris code paths.
 *   HAVE_SYS_UCRED_H        (BSD peer-credential headers)
 *   HAVE_BROKEN_MSG_CMSG_CLOEXEC
 */

/* musl-gcc already defines _GNU_SOURCE; do not redefine it. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#endif /* WAYLAND_CONFIG_H */
