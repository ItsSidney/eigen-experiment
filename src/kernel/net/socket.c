/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * socket.c - Kernel BSD socket implementation and syscall bridge.
 */

#include "socket.h"
#include "tcp.h"
#include "udp.h"
#include "icmp.h"
#include "ipv4.h"
#include "net.h"
#include "fs/vfs/vfs.h"
#include "proc/process.h"
#include "mm/kheap.h"
#include "drivers/char/keyboard.h"
#include "drivers/timer/pit.h"
#include "include/serial.h"

static struct socket_entry sockets[MAX_KERNEL_SOCKETS];
static uint16_t ephemeral_port = 50000;

/* Sockets are real file descriptors: the fd table holds an alias entry so
 * dup2, close and stdio redirection (busybox ping moves its ICMP socket to
 * fd 0) behave exactly like they do for any other fd. Resolve a user fd to
 * the kernel socket index it names, whether it arrived as a legacy raw
 * handle or as an alias. */
static int sock_idx(int fd) {
    int sfd = vfs_sock_fd(fd);
    if (sfd < 0) return -1;
    return sfd - SOCKET_FD_BASE;
}

void icmp_deliver_to_raw(uint32_t src_ip, const void *data, uint16_t len) {
    const uint8_t *bytes = (const uint8_t *)data;
    for (int i = 0; i < MAX_KERNEL_SOCKETS; i++) {
        if (sockets[i].active && sockets[i].is_icmp) {
            uint16_t copy_len = len < 2048 ? len : 2048;
            for (uint16_t b = 0; b < copy_len; b++) {
                sockets[i].icmp_rx_buf[b] = bytes[b];
            }
            sockets[i].icmp_rx_len   = copy_len;
            sockets[i].icmp_src_ip   = src_ip;
            sockets[i].icmp_has_data = 1;
        }
    }
}

static void sock_udp_callback(uint32_t src_ip, uint16_t src_port,
                              uint16_t dst_port, const uint8_t *data,
                              uint16_t len) {
    /* Deliver to the socket that owns this destination port. Each DGRAM
     * socket registers its own binding with the UDP layer, so the port is
     * exactly the demux key; falling back to "first socket" would hand NTP
     * replies to DNS's socket whenever both are open. */
    for (int i = 0; i < MAX_KERNEL_SOCKETS; i++) {
        if (!sockets[i].active || sockets[i].type != SOCK_DGRAM) continue;
        if (sockets[i].is_icmp) continue;
        if (sockets[i].local_port != dst_port) continue;
        uint16_t copy_len = len < 2048 ? len : 2048;
        for (uint16_t b = 0; b < copy_len; b++) {
            sockets[i].udp_rx_buf[b] = data[b];
        }
        sockets[i].udp_rx_len   = copy_len;
        sockets[i].udp_src_ip   = src_ip;
        sockets[i].udp_src_port = src_port;
        /* Clear before publishing the data: a reader that wakes on the flag
         * must never observe a half-filled buffer. */
        sockets[i].udp_has_data = 0;
        __asm__ volatile("" ::: "memory");
        sockets[i].udp_has_data = 1;
        return;
    }
}

void socket_init(void) {
    for (int i = 0; i < MAX_KERNEL_SOCKETS; i++) {
        sockets[i].active        = 0;
        sockets[i].refcount      = 0;
        sockets[i].tcp           = NULL;
        sockets[i].udp_has_data  = 0;
        sockets[i].icmp_has_data = 0;
        sockets[i].is_icmp       = 0;
        sockets[i].is_unix       = 0;
        sockets[i].unix_peer_idx = -1;
        sockets[i].unix_rx_buf   = NULL;
        sockets[i].unix_bound    = 0;
        sockets[i].unix_listening = 0;
        sockets[i].unix_backlog_n = 0;
    }
}

void sys_sock_incref(int idx) {
    if (idx >= 0 && idx < MAX_KERNEL_SOCKETS && sockets[idx].active) {
        sockets[idx].refcount++;
    }
}

/* Raw-handle check only: this is the cycle-breaker in the
 * is_socket_fd -> vfs_sock_fd chain, so it must not call back into either.
 * Alias resolution (fd-table entries installed by dup2/socket()/accept())
 * lives in vfs_sock_fd; full user-fd resolution in sock_idx(). */
int is_socket_fd(int fd) {
    int idx = fd - SOCKET_FD_BASE;
    return (idx >= 0 && idx < MAX_KERNEL_SOCKETS && sockets[idx].active);
}

int sock_get_type(int fd) {
    int idx = sock_idx(fd);
    if (idx < 0) return -1;
    return sockets[idx].type;
}

int sys_sock_socket(int domain, int type, int protocol) {
    (void)protocol;

    /* Mask out flags like SOCK_CLOEXEC, SOCK_NONBLOCK */
    int base_type = type & 0xFF;

    if (domain == AF_UNIX) {
        if (base_type != SOCK_STREAM && base_type != SOCK_DGRAM) return -94;
        for (int i = 0; i < MAX_KERNEL_SOCKETS; i++) {
            if (!sockets[i].active) {
                uint8_t *buf = (uint8_t *)kmalloc(UNIX_SOCK_BUF);
                if (!buf) return -12; /* -ENOMEM */
                sockets[i].active = 1;
                sockets[i].owner_pid = current_process ? current_process->pid : 0;
                sockets[i].refcount     = 1;
                sockets[i].domain       = AF_UNIX;
                sockets[i].type         = base_type;
                sockets[i].protocol     = 0;
                sockets[i].tcp          = NULL;
                sockets[i].is_icmp      = 0;
                sockets[i].is_unix      = 1;
                sockets[i].unix_peer_idx = -1;
                sockets[i].unix_rx_buf  = buf;
                sockets[i].unix_rx_head = 0;
                sockets[i].unix_rx_tail = 0;
                sockets[i].unix_rx_count = 0;
                sockets[i].unix_bound   = 0;
                sockets[i].unix_name.path[0] = '\0';
                sockets[i].unix_name.len = 0;
                sockets[i].unix_name.abstract = 0;
                sockets[i].unix_listening = 0;
                sockets[i].unix_backlog_n = 0;
                sockets[i].unix_conn.path[0] = '\0';
                sockets[i].unix_conn.len = 0;
                sockets[i].unix_conn.abstract = 0;
                sockets[i].unix_peer.path[0] = '\0';
                sockets[i].unix_peer.len = 0;
                sockets[i].unix_peer.abstract = 0;
                sockets[i].unix_dgsrc.path[0] = '\0';
                sockets[i].unix_dgsrc.len = 0;
                sockets[i].unix_dgsrc.abstract = 0;
                sockets[i].local_port   = 0;
                int alias = vfs_install_socket(i);
                if (alias < 0) {
                    kfree(buf);
                    sockets[i].unix_rx_buf = NULL;
                    sockets[i].active = 0;
                    return alias;
                }
                return alias;
            }
        }
        return -24; /* -EMFILE */
    }

    /* Only AF_INET (2) is supported; reject AF_INET6 (10) and others */
    if (domain != 2 /* AF_INET */) return -97; /* -EAFNOSUPPORT */

    for (int i = 0; i < MAX_KERNEL_SOCKETS; i++) {
        if (!sockets[i].active) {
            /* The alias installed below takes the single reference the new
             * fd owns (refcount starts at 1 via incref there), so the entry
             * only lives while some fd names it. */
            sockets[i].active = 1;
            sockets[i].owner_pid = current_process ? current_process->pid : 0;
            sockets[i].refcount     = 0;
            sockets[i].domain       = domain;
            sockets[i].type         = base_type;
            sockets[i].protocol     = protocol;
            sockets[i].udp_has_data = 0;
            sockets[i].local_port   = 0; /* unbound until a path claims one */

            if (base_type == SOCK_STREAM) {
                sockets[i].tcp = tcp_alloc_conn();
                if (!sockets[i].tcp) {
                    sockets[i].active = 0;
                    return -12; /* -ENOMEM */
                }
                sockets[i].is_icmp = 0;
            } else if (base_type == SOCK_DGRAM) {
                /* SOCK_DGRAM with IPPROTO_ICMP is used for unprivileged ping */
                if (protocol == 1 /* IPPROTO_ICMP */) {
                    sockets[i].tcp = NULL;
                    sockets[i].is_icmp = 1;
                    sockets[i].icmp_has_data = 0;
                } else {
                    sockets[i].tcp = NULL;
                    sockets[i].is_icmp = 0;
                    sockets[i].local_port = ephemeral_port++;
                    sockets[i].remote_ip = 0;
                    sockets[i].remote_port = 0;
                    sockets[i].udp_src_ip = 0;
                    sockets[i].udp_src_port = 0;
                    sockets[i].udp_rx_len = 0;
                    if (ephemeral_port > 65000) ephemeral_port = 50000;
                    udp_bind_handler(sockets[i].local_port, sock_udp_callback);
                }
            } else if (base_type == SOCK_RAW) {
                /* SOCK_RAW is used for ping (IPPROTO_ICMP) */
                sockets[i].tcp = NULL;
                sockets[i].is_icmp = (protocol == 1 || protocol == 58);
                sockets[i].icmp_has_data = 0;
                /* For raw IP, no UDP port needed */
            } else {
                sockets[i].tcp = NULL;
                sockets[i].is_icmp = 0;
            }

            int alias = vfs_install_socket(i);
            if (alias < 0) {
                /* Roll the half-created entry back: it would otherwise hold
                 * a UDP binding or a TCP connection for the life of the
                 * kernel with no fd anywhere to close it. */
                if (sockets[i].is_unix && sockets[i].unix_rx_buf) {
                    kfree(sockets[i].unix_rx_buf);
                    sockets[i].unix_rx_buf = NULL;
                }
                if (sockets[i].tcp) {
                    tcp_free_conn(sockets[i].tcp);
                    sockets[i].tcp = NULL;
                }
                if (base_type == SOCK_DGRAM && !sockets[i].is_icmp &&
                    sockets[i].local_port)
                    udp_unbind_handler(sockets[i].local_port);
                sockets[i].active = 0;
                return alias;
            }
            return alias;
        }
    }
    return -24; /* -EMFILE */
}

int sys_sock_socketpair(int domain, int type, int protocol, int sv[2]) {
    (void)protocol;
    if (!sv) return -14; /* -EFAULT */
    if (domain != AF_UNIX) return -97; /* -EAFNOSUPPORT */

    int base_type = type & 0xFF;
    if (base_type != SOCK_STREAM && base_type != SOCK_DGRAM) return -94; /* -ESOCKTNOSUPPORT */

    int a = -1, b = -1;
    for (int i = 0; i < MAX_KERNEL_SOCKETS; i++) {
        if (!sockets[i].active) {
            if (a < 0) a = i;
            else { b = i; break; }
        }
    }
    if (a < 0 || b < 0) return -24; /* -EMFILE */

    uint8_t *bufa = (uint8_t *)kmalloc(UNIX_SOCK_BUF);
    uint8_t *bufb = (uint8_t *)kmalloc(UNIX_SOCK_BUF);
    if (!bufa || !bufb) {
        if (bufa) kfree(bufa);
        if (bufb) kfree(bufb);
        return -12; /* -ENOMEM */
    }

    sockets[a].active = 1;
    sockets[a].owner_pid = current_process ? current_process->pid : 0;
    sockets[a].refcount     = 0; /* vfs_install_socket takes the fd's reference */
    sockets[a].domain       = domain;
    sockets[a].type         = SOCK_STREAM;
    sockets[a].protocol     = 0;
    sockets[a].tcp          = NULL;
    sockets[a].is_icmp      = 0;
    sockets[a].is_unix      = 1;
    sockets[a].unix_peer_idx = b;
    sockets[a].unix_rx_buf  = bufa;
    sockets[a].unix_rx_head = 0;
    sockets[a].unix_rx_tail = 0;
    sockets[a].unix_rx_count = 0;

    sockets[b].active = 1;
    sockets[b].owner_pid = current_process ? current_process->pid : 0;
    sockets[b].refcount     = 0; /* vfs_install_socket takes the fd's reference */
    sockets[b].domain       = domain;
    sockets[b].type         = SOCK_STREAM;
    sockets[b].protocol     = 0;
    sockets[b].tcp          = NULL;
    sockets[b].is_icmp      = 0;
    sockets[b].is_unix      = 1;
    sockets[b].unix_peer_idx = a;
    sockets[b].unix_rx_buf  = bufb;
    sockets[b].unix_rx_head = 0;
    sockets[b].unix_rx_tail = 0;
    sockets[b].unix_rx_count = 0;

    int fa = vfs_install_socket(a);
    if (fa < 0) {
        kfree(bufa); kfree(bufb);
        sockets[a].active = 0;
        return fa;
    }
    int fb = vfs_install_socket(b);
    if (fb < 0) {
        /* fd_close has no alias yet for fa, so undo both sockets here. */
        kfree(bufa); kfree(bufb);
        sockets[a].active = 0;
        sockets[b].active = 0;
        return fb;
    }
    sv[0] = fa;
    sv[1] = fb;
    return 0;
}

/* True while the process that created a socket still exists and has not
 * exited. A socket whose owner is gone can never be reached by any fd, so it
 * is stale: its name must be reclaimable even if the owner's descriptor
 * table was never swept (a signal-kill path, a corrupted teardown, ...).
 * This check is what makes rebinding a compositor's socket reliable rather
 * than dependent on every exit path closing fds. */
static int sock_owner_alive(uint32_t pid) {
    if (pid == 0) return 0;
    for (struct process *p = process_iter(NULL); p; p = process_iter(p)) {
        if (p->pid == pid) return p->exited ? 0 : 1;
    }
    return 0; /* not on the list any more: definitely gone */
}

/* Drop a socket whose owner has died, releasing its bound name. */
static void sock_reclaim_if_dead(int idx) {
    if (!sockets[idx].active) return;
    if (sock_owner_alive(sockets[idx].owner_pid)) return;
    sockets[idx].active      = 0;
    sockets[idx].refcount    = 0;
    sockets[idx].unix_bound  = 0;
    sockets[idx].unix_listening = 0;
    sockets[idx].unix_backlog_n = 0;
    sockets[idx].local_port  = 0;
    if (sockets[idx].unix_rx_buf) {
        kfree(sockets[idx].unix_rx_buf);
        sockets[idx].unix_rx_buf = NULL;
    }
}

/* ── AF_UNIX filesystem + abstract names ── */

static int unix_streq(const char *a, const char *b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static void unix_addr_copy(struct unix_addr *dst, const char *path, int len, int abstract) {
    if (len > UNIX_PATH_MAX) len = UNIX_PATH_MAX;
    for (int i = 0; i < len; i++) dst->path[i] = path[i];
    if (len < UNIX_PATH_MAX) dst->path[len] = '\0';
    dst->len = len;
    dst->abstract = abstract;
}

/* Split a user sockaddr_un into raw path bytes + namespace. */
static int unix_parse_addr(const struct sockaddr_un *addr, uint32_t addrlen,
                           char *path_out, int *len_out, int *abstract_out) {
    if (!addr || addrlen < 3 || addr->sun_family != AF_UNIX) return -22;
    uint32_t n = addrlen - 2;
    if (n > 107) n = 107;
    if (addr->sun_path[0] == '\0') {
        for (uint32_t i = 0; i < n; i++) path_out[i] = addr->sun_path[i];
        if (n < UNIX_PATH_MAX) path_out[n] = '\0';
        *len_out = (int)n;
        *abstract_out = 1;
        return 0;
    }
    uint32_t i = 0;
    while (i < n && i < UNIX_PATH_MAX - 1 && addr->sun_path[i]) {
        path_out[i] = addr->sun_path[i];
        i++;
    }
    path_out[i] = '\0';
    if (i == 0) return -22;
    *len_out = (int)i;
    *abstract_out = 0;
    return 0;
}

static int unix_names_match(struct unix_addr *a, const char *path, int len, int abstract) {
    if (a->abstract != abstract || a->len != len) return 0;
    if (abstract) {
        for (int k = 0; k < len; k++) {
            if (a->path[k] != path[k]) return 0;
        }
        return 1;
    }
    return unix_streq(a->path, path);
}

static int unix_lookup(const char *path, int len, int abstract, int need_listen) {
    for (int i = 0; i < MAX_KERNEL_SOCKETS; i++) {
        if (!sockets[i].active || !sockets[i].is_unix || !sockets[i].unix_bound)
            continue;
        if (!unix_names_match(&sockets[i].unix_name, path, len, abstract)) continue;
        if (need_listen && !sockets[i].unix_listening) continue;
        return i;
    }
    return -1;
}

static int unix_new_entry(int type) {
    int idx = -1;
    for (int i = 0; i < MAX_KERNEL_SOCKETS; i++) {
        if (!sockets[i].active) {
            idx = i;
            break;
        }
    }
    if (idx < 0) return -24;
    uint8_t *buf = (uint8_t *)kmalloc(UNIX_SOCK_BUF);
    if (!buf) return -12;
    sockets[idx].active = 1;
    sockets[idx].owner_pid = current_process ? current_process->pid : 0;
    sockets[idx].refcount     = 0; /* vfs_install_socket takes the fd's reference */
    sockets[idx].domain       = AF_UNIX;
    sockets[idx].type         = type;
    sockets[idx].protocol     = 0;
    sockets[idx].tcp          = NULL;
    sockets[idx].is_icmp      = 0;
    sockets[idx].is_unix      = 1;
    sockets[idx].unix_peer_idx = -1;
    sockets[idx].unix_rx_buf  = buf;
    sockets[idx].unix_rx_head = 0;
    sockets[idx].unix_rx_tail = 0;
    sockets[idx].unix_rx_count = 0;
    sockets[idx].unix_bound   = 0;
    sockets[idx].unix_listening = 0;
    sockets[idx].unix_backlog_n = 0;
    sockets[idx].unix_name.len = 0;
    sockets[idx].unix_name.abstract = 0;
    sockets[idx].unix_name.path[0] = '\0';
    sockets[idx].unix_conn.len = 0;
    sockets[idx].unix_conn.abstract = 0;
    sockets[idx].unix_conn.path[0] = '\0';
    sockets[idx].unix_peer.len = 0;
    sockets[idx].unix_peer.abstract = 0;
    sockets[idx].unix_peer.path[0] = '\0';
    sockets[idx].unix_dgsrc.len = 0;
    sockets[idx].unix_dgsrc.abstract = 0;
    sockets[idx].unix_dgsrc.path[0] = '\0';
    return idx;
}

static void unix_ring_push(int idx, const uint8_t *src, size_t len) {
    struct socket_entry *dst = &sockets[idx];
    size_t done = 0;
    while (done < len) {
        size_t space = UNIX_SOCK_BUF - dst->unix_rx_count;
        size_t chunk = len - done < space ? len - done : space;
        for (size_t k = 0; k < chunk; k++) {
            dst->unix_rx_buf[dst->unix_rx_head] = src[done + k];
            dst->unix_rx_head = (dst->unix_rx_head + 1) % UNIX_SOCK_BUF;
        }
        dst->unix_rx_count += (uint16_t)chunk;
        done += chunk;
    }
}

/* Fill a user sockaddr_un; reports the full length back like Linux. */
static int unix_fill_addr(struct unix_addr *name, struct sockaddr_un *addr, uint32_t *addrlen) {
    uint32_t sl = (uint32_t)name->len;
    uint32_t need = name->abstract ? 2 + sl : (sl ? 2 + sl + 1 : 2);
    if (!addr || !addrlen) return -14;
    uint32_t out = *addrlen < need ? *addrlen : need;
    uint8_t *d = (uint8_t *)addr;
    for (uint32_t i = 0; i < out; i++) {
        if (i < 2) d[i] = (uint8_t)(AF_UNIX >> (i * 8));
        else if (name->abstract) d[i] = (uint8_t)name->path[i - 2];
        else if (i - 2 < sl) d[i] = (uint8_t)name->path[i - 2];
        else d[i] = 0;
    }
    *addrlen = need;
    return 0;
}

int sys_sock_bind(int fd, const struct sockaddr_un *addr, uint32_t addrlen) {
    int idx = sock_idx(fd);
    if (idx < 0) return -9; /* -EBADF */
    struct socket_entry *sock = &sockets[idx];
    if (!sock->is_unix) {
        /* AF_INET bind: claim local_port and move the UDP demux binding
         * (creation only holds a provisional ephemeral one). struct
         * sockaddr_in: family u16 @0, port BE @2, addr @4. */
        const uint8_t *b = (const uint8_t *)addr;
        uint16_t port;
        if (addrlen < 8) return -22;
        if (b[0] != 2 || b[1] != 0) return -97; /* AF_INET only */
        port = (uint16_t)((uint16_t)b[2] << 8 | b[3]);
        if (port == 0) {
            /* bind(0): fresh ephemeral, like creation */
            port = ephemeral_port++;
            if (ephemeral_port > 65000) ephemeral_port = 50000;
        } else {
            for (int i = 0; i < MAX_KERNEL_SOCKETS; i++) {
                if (i == idx || !sockets[i].active || sockets[i].is_unix ||
                    sockets[i].is_icmp || sockets[i].local_port == 0)
                    continue;
                if (sockets[i].local_port == port)
                    return -98; /* -EADDRINUSE */
            }
        }
        if (sock->type == SOCK_DGRAM && !sock->is_icmp) {
            if (sock->local_port)
                udp_unbind_handler(sock->local_port);
            udp_bind_handler(port, sock_udp_callback);
        }
        sock->local_port = port;
        return 0;
    }
    if (sock->type != SOCK_STREAM && sock->type != SOCK_DGRAM) return -94;
    if (sock->unix_bound) return -22; /* -EINVAL: already bound */
    char path[UNIX_PATH_MAX];
    int len = 0, abstract = 0;
    int r = unix_parse_addr(addr, addrlen, path, &len, &abstract);
    if (r < 0) return r;
    for (int i = 0; i < MAX_KERNEL_SOCKETS; i++) {
        if (i == idx || !sockets[i].active) continue;
        sock_reclaim_if_dead(i);          /* a dead owner's name is free */
        if (!sockets[i].active) continue;
        if (sockets[i].is_unix && sockets[i].unix_bound &&
            unix_names_match(&sockets[i].unix_name, path, len, abstract))
            return -98; /* -EADDRINUSE */
    }
    if (!abstract) {
        /* A leftover socket node from a dead process must not block a new
         * bind: Linux lets the caller unlink and retry. Stale lockfiles are
         * handled the same way, so no special case is needed for the
         * /tmp/wayland-N.sock left behind when the compositor exits. */
        int r2 = vfs_mksock(path);
        if (r2 == -17) {
            /* Path exists. If it is a plain socket node, remove it and try
             * again; anything else (dir/file) is an honest conflict. */
            struct vfs_node *stale = vfs_lookup(path);
            if (stale && stale->type == VFS_SOCKET) {
                vfs_unlink(path);
                r2 = vfs_mksock(path);
            }
        }
        if (r2 < 0) return r2;
    }
    unix_addr_copy(&sock->unix_name, path, len, abstract);
    sock->unix_bound = 1;
    return 0;
}

int sys_sock_listen(int fd, int backlog) {
    (void)backlog;
    int idx = sock_idx(fd);
    if (idx < 0) return -9; /* -EBADF */
    struct socket_entry *sock = &sockets[idx];
    if (!sock->is_unix) return 0;
    if (sock->type != SOCK_STREAM) return -95; /* -EOPNOTSUPP */
    sock->unix_listening = 1;
    return 0;
}

int sys_sock_accept(int fd, struct sockaddr_un *addr, uint32_t *addrlen) {
    int idx = sock_idx(fd);
    if (idx < 0) return -9; /* -EBADF */
    struct socket_entry *sock = &sockets[idx];
    if (!sock->is_unix) return 0;
    if (!sock->unix_listening) return -22; /* -EINVAL */
    while (sock->unix_backlog_n == 0) {
        tty_poll_signal();
        if (current_process &&
            (current_process->sig_pending & ~current_process->sig_blocked))
            return -4; /* -EINTR */
        __asm__ volatile("pause");
    }
    int s = sock->unix_backlog_q[0];
    for (int i = 1; i < sock->unix_backlog_n; i++)
        sock->unix_backlog_q[i - 1] = sock->unix_backlog_q[i];
    sock->unix_backlog_n--;
    if (addr) {
        /* Peer is the connector's bound name, if it has one. */
        struct unix_addr *peer = &sockets[s].unix_peer;
        if (peer->len == 0 && !peer->abstract) {
            uint32_t need = 2;
            if (!addrlen) return -14;
            uint32_t out = *addrlen < need ? *addrlen : need;
            uint8_t *d = (uint8_t *)addr;
            for (uint32_t i = 0; i < out; i++) d[i] = (uint8_t)(AF_UNIX >> (i * 8));
            *addrlen = need;
        } else if (unix_fill_addr(peer, addr, addrlen) < 0) {
            return -14;
        }
    }
    int sfd = vfs_install_socket(s);
    if (sfd < 0) {
        /* The queued connection dies with no fd to serve it. */
        if (sockets[s].unix_rx_buf) kfree(sockets[s].unix_rx_buf);
        sockets[s].active = 0;
        return sfd;
    }
    return sfd;
}

int sys_sock_shutdown(int fd, int how) {
    (void)how;
    if (sock_idx(fd) < 0) return -9; /* -EBADF */
    return 0;
}

int sys_sock_getsockname(int fd, struct sockaddr_un *addr, uint32_t *addrlen) {
    int idx = sock_idx(fd);
    if (idx < 0) return -9; /* -EBADF */
    struct socket_entry *sock = &sockets[idx];
    if (!sock->is_unix) return 0;
    return unix_fill_addr(&sock->unix_name, addr, addrlen);
}

int sys_sock_getpeername(int fd, struct sockaddr_un *addr, uint32_t *addrlen) {
    int idx = sock_idx(fd);
    if (idx < 0) return -9; /* -EBADF */
    struct socket_entry *sock = &sockets[idx];
    if (!sock->is_unix) return 0;
    if (sock->unix_conn.len > 0 || sock->unix_conn.abstract)
        return unix_fill_addr(&sock->unix_conn, addr, addrlen);
    int peer = sock->unix_peer_idx;
    if (peer < 0 || peer >= MAX_KERNEL_SOCKETS || !sockets[peer].active)
        return -107; /* -ENOTCONN */
    if (sockets[peer].unix_name.len == 0 && !sockets[peer].unix_name.abstract) {
        if (!addr || !addrlen) return -14;
        uint32_t out = *addrlen < 2 ? *addrlen : 2;
        uint8_t *d = (uint8_t *)addr;
        for (uint32_t i = 0; i < out; i++) d[i] = (uint8_t)(AF_UNIX >> (i * 8));
        *addrlen = 2;
        return 0;
    }
    return unix_fill_addr(&sockets[peer].unix_name, addr, addrlen);
}

int sys_sock_connect(int fd, const struct sockaddr_in *addr, uint32_t addrlen) {
    int idx = sock_idx(fd);
    if (idx < 0 || !addr) return -9; /* -EBADF */

    struct socket_entry *sock = &sockets[idx];

    if (sock->is_unix) {
        const struct sockaddr_un *un = (const struct sockaddr_un *)addr;
        char path[UNIX_PATH_MAX];
        int len = 0, abstract = 0;
        int r = unix_parse_addr(un, addrlen, path, &len, &abstract);
        if (r < 0) return r;
        if (sock->type == SOCK_DGRAM) {
            unix_addr_copy(&sock->unix_conn, path, len, abstract);
            return 0;
        }
        if (sock->unix_peer_idx >= 0) return -106; /* -EISCONN */
        int l = unix_lookup(path, len, abstract, 1);
        if (l < 0) return -111; /* -ECONNREFUSED */
        if (sockets[l].unix_backlog_n >= UNIX_BACKLOG_MAX) return -11;
        int s = unix_new_entry(SOCK_STREAM);
        if (s < 0) return s;
        sockets[s].unix_peer_idx = idx;
        sock->unix_peer_idx = s;
        unix_addr_copy(&sock->unix_conn, path, len, abstract);
        unix_addr_copy(&sockets[s].unix_name, sockets[l].unix_name.path,
                       sockets[l].unix_name.len, sockets[l].unix_name.abstract);
        if (sock->unix_bound)
            unix_addr_copy(&sockets[s].unix_peer, sock->unix_name.path,
                           sock->unix_name.len, sock->unix_name.abstract);
        sockets[l].unix_backlog_q[sockets[l].unix_backlog_n++] = s;
        return 0;
    }

    (void)addrlen;
    sock->remote_ip   = addr->sin_addr;
    sock->remote_port = ntohs(addr->sin_port);

    if (sock->type == SOCK_STREAM && sock->tcp) {
        int ret = tcp_connect(sock->tcp, sock->remote_ip, sock->remote_port, 5000);
        return ret == 0 ? 0 : -111; /* -ECONNREFUSED */
    }

    return 0;
}

int sys_sock_send(int fd, const void *buf, size_t len, int flags) {
    return sys_sock_sendto(fd, buf, len, flags, NULL, 0);
}

int sys_sock_sendto(int fd, const void *buf, size_t len, int flags, const struct sockaddr_in *dest_addr, uint32_t addrlen) {
    (void)flags;
    (void)addrlen;
    int idx = sock_idx(fd);
    if (idx < 0 || !buf) return -9; /* -EBADF */

    struct socket_entry *sock = &sockets[idx];

    /* AF_UNIX socketpair: write into the peer's ring (blocking). */
    if (sock->is_unix) {
        if (!buf) return -14;
        if (sock->type == SOCK_DGRAM) {
            const struct sockaddr_un *un = (const struct sockaddr_un *)dest_addr;
            char dpath[UNIX_PATH_MAX];
            int dlen = 0, dabstract = 0;
            if (dest_addr) {
                if (*(const uint16_t *)dest_addr != AF_UNIX) return -22;
                int r = unix_parse_addr(un, addrlen, dpath, &dlen, &dabstract);
                if (r < 0) return r;
            } else if (sock->unix_conn.len > 0 || sock->unix_conn.abstract) {
                dlen = sock->unix_conn.len;
                dabstract = sock->unix_conn.abstract;
                for (int i = 0; i < dlen; i++) dpath[i] = sock->unix_conn.path[i];
                if (dlen < UNIX_PATH_MAX) dpath[dlen] = '\0';
            } else {
                return -107; /* -ENOTCONN */
            }
            int t = -1;
            for (int i = 0; i < MAX_KERNEL_SOCKETS; i++) {
                if (!sockets[i].active || !sockets[i].is_unix ||
                    sockets[i].type != SOCK_DGRAM || !sockets[i].unix_bound)
                    continue;
                if (!unix_names_match(&sockets[i].unix_name, dpath, dlen, dabstract))
                    continue;
                t = i;
                break;
            }
            if (t < 0) return -111; /* -ECONNREFUSED */
            if (len > UNIX_SOCK_BUF) return -90; /* -EMSGSIZE */
            if (sockets[t].unix_rx_count + len > UNIX_SOCK_BUF) return -11;
            unix_ring_push(t, (const uint8_t *)buf, len);
            if (sock->unix_bound)
                unix_addr_copy(&sockets[t].unix_dgsrc, sock->unix_name.path,
                               sock->unix_name.len, sock->unix_name.abstract);
            else {
                sockets[t].unix_dgsrc.len = 0;
                sockets[t].unix_dgsrc.abstract = 0;
                sockets[t].unix_dgsrc.path[0] = '\0';
            }
            return (int)len;
        }
        int peer = sock->unix_peer_idx;
        if (peer < 0 || peer >= MAX_KERNEL_SOCKETS || !sockets[peer].active) {
            return -32; /* -EPIPE */
        }
        struct socket_entry *dst = &sockets[peer];
        const uint8_t *src = (const uint8_t *)buf;
        size_t done = 0;
        while (done < len) {
            if (!sockets[peer].active) return done ? (int)done : -32;
            while (dst->unix_rx_count >= UNIX_SOCK_BUF) {
                if (!sockets[peer].active) return done ? (int)done : -32;
                tty_poll_signal();
                if (current_process &&
                    (current_process->sig_pending & ~current_process->sig_blocked))
                    return done ? (int)done : -4;
                __asm__ volatile("pause");
            }
            uint16_t space = UNIX_SOCK_BUF - dst->unix_rx_count;
            size_t chunk = len - done < space ? len - done : space;
            for (size_t i = 0; i < chunk; i++) {
                dst->unix_rx_buf[dst->unix_rx_head] = src[done + i];
                dst->unix_rx_head = (dst->unix_rx_head + 1) % UNIX_SOCK_BUF;
            }
            dst->unix_rx_count += (uint16_t)chunk;
            done += chunk;
        }
        return (int)done;
    }

    /* ICMP raw / DGRAM ping: buf already contains ICMP header, send as-is */
    if (sock->is_icmp) {
        uint32_t dst_ip = sock->remote_ip;
        if (dest_addr) dst_ip = dest_addr->sin_addr;
        /* Also handle raw IP sendto with sockaddr_in */
        if (dst_ip == 0) return -22; /* -EINVAL */
        int ret = ipv4_send(dst_ip, 1 /* IP_PROTO_ICMP */, buf, (uint16_t)len);
        return ret == 0 ? (int)len : -1;
    }

    if (sock->type == SOCK_DGRAM) {
        uint32_t dst_ip   = sock->remote_ip;
        uint16_t dst_port = sock->remote_port;
        if (dest_addr) {
            dst_ip   = dest_addr->sin_addr;
            dst_port = ntohs(dest_addr->sin_port);
        }
        int ret = udp_send(dst_ip, sock->local_port, dst_port, buf, (uint16_t)len);
        return ret == 0 ? (int)len : -1;
    } else if (sock->type == SOCK_STREAM && sock->tcp) {
        return tcp_send(sock->tcp, buf, (uint16_t)len);
    }

    return -88; /* -ENOTSOCK */
}

int sys_sock_recv(int fd, void *buf, size_t len, int flags) {
    return sys_sock_recvfrom(fd, buf, len, flags, NULL, NULL);
}

int sys_sock_recvfrom(int fd, void *buf, size_t len, int flags, struct sockaddr_in *src_addr, uint32_t *addrlen) {
    int idx = sock_idx(fd);
    if (idx < 0 || !buf) return -9; /* -EBADF */

    struct socket_entry *sock = &sockets[idx];
    int nonblock = (flags & 0x40) != 0; /* MSG_DONTWAIT = 0x40 */

    /* AF_UNIX socketpair: read from our own ring (blocking, EOF on peer close). */
    if (sock->is_unix) {
        if (len == 0) return 0;
        if (sock->type == SOCK_DGRAM) {
            while (sock->unix_rx_count == 0) {
                tty_poll_signal();
                if (current_process &&
                    (current_process->sig_pending & ~current_process->sig_blocked))
                    return -4; /* -EINTR */
                __asm__ volatile("pause");
            }
        } else {
            while (sock->unix_rx_count == 0) {
                int peer = sock->unix_peer_idx;
                if (peer < 0 || peer >= MAX_KERNEL_SOCKETS || !sockets[peer].active) {
                    return 0; /* EOF */
                }
                if (nonblock) return -11; /* -EAGAIN */
                tty_poll_signal();
                if (current_process &&
                    (current_process->sig_pending & ~current_process->sig_blocked))
                    return -4; /* -EINTR */
                __asm__ volatile("pause");
            }
        }
        uint16_t copy_len = sock->unix_rx_count < len ? sock->unix_rx_count : (uint16_t)len;
        uint8_t *d = (uint8_t *)buf;
        for (uint16_t i = 0; i < copy_len; i++) {
            d[i] = sock->unix_rx_buf[sock->unix_rx_tail];
            sock->unix_rx_tail = (sock->unix_rx_tail + 1) % UNIX_SOCK_BUF;
        }
        sock->unix_rx_count -= copy_len;
        if (src_addr && sock->domain == AF_UNIX) {
            struct unix_addr anon;
            struct unix_addr *who = &anon;
            anon.len = 0;
            anon.abstract = 0;
            anon.path[0] = '\0';
            if (sock->type == SOCK_DGRAM) {
                who = &sock->unix_dgsrc;
            } else {
                int peer = sock->unix_peer_idx;
                if (peer >= 0 && peer < MAX_KERNEL_SOCKETS && sockets[peer].active &&
                    (sockets[peer].unix_name.len > 0 || sockets[peer].unix_name.abstract))
                    who = &sockets[peer].unix_name;
            }
            if (unix_fill_addr(who, (struct sockaddr_un *)src_addr, addrlen) < 0)
                return -14;
        }
        return (int)copy_len;
    }

    if (sock->is_icmp) {
        uint64_t start = timer_get_ticks();
        while (!sock->icmp_has_data) {
            net_poll();
            tty_poll_signal();
            if (sock->icmp_has_data) break;
            if (current_process &&
                (current_process->sig_pending & ~current_process->sig_blocked))
                return -4; /* -EINTR */
            if (nonblock) return -11; /* -EAGAIN */
            if (timer_get_ticks() - start > 500) { /* 5 sec timeout for ping */
                return -11;
            }
            __asm__ volatile("pause");
        }
        uint16_t copy_len = sock->icmp_rx_len < len ? sock->icmp_rx_len : (uint16_t)len;
        uint8_t *d = (uint8_t *)buf;
        for (uint16_t i = 0; i < copy_len; i++) d[i] = sock->icmp_rx_buf[i];
        if (src_addr) {
            uint8_t *z = (uint8_t *)src_addr;
            for (uint32_t i = 0; i < sizeof(struct sockaddr_in); i++) z[i] = 0;
            src_addr->sin_family = AF_INET;
            src_addr->sin_addr   = sock->icmp_src_ip;
            src_addr->sin_port   = 0;
            if (addrlen) *addrlen = sizeof(struct sockaddr_in);
        }
        sock->icmp_has_data = 0;
        return (int)copy_len;
    }

    if (sock->type == SOCK_DGRAM) {
        uint64_t start = timer_get_ticks();
        int have_data = sock->udp_has_data;
        while (!have_data) {
            net_poll();
            tty_poll_signal();
            if ((have_data = sock->udp_has_data)) break;
            if (current_process &&
                (current_process->sig_pending & ~current_process->sig_blocked))
                return -4; /* -EINTR */
            if (nonblock) return -11; /* -EAGAIN */
            if (timer_get_ticks() - start > 300) { /* 3 second timeout */
                return -11;
            }
            __asm__ volatile("pause");
        }

        uint16_t copy_len = sock->udp_rx_len < len ? sock->udp_rx_len : (uint16_t)len;
        uint8_t *d = (uint8_t *)buf;
        for (uint16_t i = 0; i < copy_len; i++) d[i] = sock->udp_rx_buf[i];

        if (src_addr) {
            /* Zero the whole address (musl memcmps sin_zero too). */
            uint8_t *z = (uint8_t *)src_addr;
            for (uint32_t i = 0; i < sizeof(struct sockaddr_in); i++) {
                z[i] = 0;
            }
            src_addr->sin_family = AF_INET;
            src_addr->sin_addr   = sock->udp_src_ip;
            src_addr->sin_port   = htons(sock->udp_src_port);
            if (addrlen) *addrlen = sizeof(struct sockaddr_in);
        }

        sock->udp_has_data = 0;
        return (int)copy_len;
    } else if (sock->type == SOCK_STREAM && sock->tcp) {
        /* tcp_recv takes a 16-bit length: clamp huge reads instead of
         * truncating (a multiple of 64K would otherwise become 0 = EOF). */
        uint16_t chunk = (len == 0) ? 0 : (len > 0xFFFF ? 0xFFFF : (uint16_t)len);
        return tcp_recv(sock->tcp, buf, chunk, nonblock);
    }

    return -88;
}

int sys_sock_close(int fd) {
    int idx = sock_idx(fd);
    if (idx < 0) return -9; /* -EBADF */

    struct socket_entry *sock = &sockets[idx];
    /* Shared (dup'd/aliased) entries die on the last close only. */
    if (--sock->refcount > 0) return 0;
    if (sock->refcount < 0) {
        sock->refcount = 0;
        return 0;
    }
    if (sock->type == SOCK_STREAM && sock->tcp) {
        tcp_close(sock->tcp);
        tcp_free_conn(sock->tcp);
        sock->tcp = NULL;
    } else if (sock->type == SOCK_DGRAM) {
        /* Only a real UDP socket owns a port binding. ICMP sockets have no
         * local_port, and unbinding a stale one here would tear down the
         * DNS/NTP binding that happens to sit on that port number. */
        if (!sock->is_icmp && sock->local_port) {
            udp_unbind_handler(sock->local_port);
            sock->local_port = 0;
        }
    } else if (sock->is_unix) {
        for (int i = 0; i < sock->unix_backlog_n; i++) {
            int q = sock->unix_backlog_q[i];
            if (q >= 0 && q < MAX_KERNEL_SOCKETS && sockets[q].active) {
                if (sockets[q].unix_rx_buf) kfree(sockets[q].unix_rx_buf);
                sockets[q].active = 0;
            }
        }
        sock->unix_backlog_n = 0;
        if (sock->unix_rx_buf) {
            kfree(sock->unix_rx_buf);
            sock->unix_rx_buf = NULL;
        }
        sock->unix_peer_idx = -1;
    }

    sock->active = 0;
    return 0;
}

int sys_sock_poll(int fd, int events) {
    int idx = sock_idx(fd);
    if (idx < 0) return 0;

    struct socket_entry *sock = &sockets[idx];
    int revents = 0;

    if (sock->is_unix) {
        int peer = sock->unix_peer_idx;
        int peer_alive = (peer >= 0 && peer < MAX_KERNEL_SOCKETS && sockets[peer].active);
        if (peer_alive) revents |= 0x0004; /* POLLOUT */
        else revents |= 0x0008; /* POLLERR */
        if (sock->unix_rx_count > 0) revents |= 0x0001; /* POLLIN */
        else if (!peer_alive) revents |= 0x0010 | 0x0001; /* POLLHUP + POLLIN for EOF */
    } else if (sock->type == SOCK_STREAM && sock->tcp) {
        if (sock->tcp->state == TCP_STATE_ESTABLISHED) {
            revents |= 0x0004; /* POLLOUT */
        }
        if (sock->tcp->rx_head != sock->tcp->rx_tail) {
            revents |= 0x0001; /* POLLIN */
        }
        if (sock->tcp->state == TCP_STATE_CLOSED ||
            sock->tcp->state == TCP_STATE_CLOSE_WAIT) {
            revents |= 0x0010; /* POLLHUP */
            if (sock->tcp->rx_head == sock->tcp->rx_tail) {
                revents |= 0x0001; /* POLLIN -> readable EOF */
            }
        }
    } else if (sock->is_icmp) {
        revents |= 0x0004; /* POLLOUT */
        if (sock->icmp_has_data) revents |= 0x0001; /* POLLIN */
    } else if (sock->type == SOCK_DGRAM) {
        revents |= 0x0004; /* POLLOUT (UDP is always writable) */
        if (sock->udp_has_data) {
            revents |= 0x0001; /* POLLIN */
        }
    }

    return revents & events;
}
