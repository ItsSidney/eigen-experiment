# EigenOS TODO — Kernel Work for Linux Compatibility

This list focuses only on kernel-level work needed for musl/glibc compatibility.
Userspace ports (games, tmux, vim) are a separate concern once the kernel syscalls
are solid.

---

## vDSO (Virtual Dynamic Shared Object)

Without vDSO, musl/glibc cannot resolve clock_gettime/gettimeofday/time/getcpu
without a syscall trap — slower, and some binaries refuse to run.

- [ ] Build proper vDSO ELF shared object with GNU_HASH, DYNSYM, dynstr sections
  - musl's __vdsosym() parses the vDSO ELF at process startup — malformed ELF causes GPF
  - Must have: PT_LOAD segments, PT_DYNAMIC, DT_GNU_HASH, DT_STRTAB, DT_SYMTAB
  - Compile via musl-gcc -shared, not custom linker scripts
- [ ] Embed vDSO blob in kernel: compile .so → objcopy -O binary → C array → link
- [ ] Map vDSO + shared clock page at fixed user VAs:
  - vDSO at 0x7FFFF00000
  - Clock page at 0x7FFFFFE000 (read-only user)
- [ ] Wire into process_create_from_elf(), sys_execve(), sys_fork() (via vmm_clone)
- [ ] Add AT_SYSINFO (32) and AT_SYSINFO_EHDR (33) to auxv
- [ ] Update clock page from PIT interrupt handler with lock-free generation counter
- [ ] Verify: clock_gettime, gettimeofday, time, getcpu work without syscall

---

## ioctl Subsystem

Needed for terminal control, networking, and most real Linux binaries.

- [ ] TCGETS / TCSETS / TCSETSW / TCSETSF (termios)
- [ ] TCFLSH (flush input/output)
- [ ] TIOCGWINSZ / TIOCSWINSZ (window size — ncurses, vim, top)
- [ ] FIONREAD (bytes available to read — readline, shells)
- [ ] FIONBIO / FIOASYNC
- [ ] SIOCGIFADDR, SIOCGIFHWADDR, SIOCGIFMTU (network tools)

---

## PTY Subsystem

Needed by tmux, screen, sshd, and any app that spawns a sub-shell in a new terminal.

- [x] /dev/ptmx character device (open handler allocates a new PTY pair number N)
- [x] /dev/pts/N slave side (termios struct, window size, read/write ring buffers)
- [x] Master side (anonymous per-process fd, gets returned from open(/dev/ptmx))
- [x] Two linked ring buffers: master write → slave read, slave write → master read
- [x] grantpt() — sets ownership/permissions on the slave node (metadata setter, not syscall)
- [x] ptsname() — returns "/dev/pts/N" path for the slave
- [x] unlockpt() — clears the lock on the slave (metadata setter)
- [x] Termios state per PTY pair (c_iflag, c_oflag, c_cflag, c_lflag, c_cc, speed)
- [x] PTY hangup on master close (SIGHUP to slave's controlling process group)

---

## sys_clone + TLS

fork() works. Threads and modern libc runtime need clone with flags.

- [ ] sys_clone (56) with CLONE_VM, CLONE_FS, CLONE_FILES, CLONE_SIGHAND, CLONE_THREAD
- [ ] sys_clone3 (glibc 2.34+ uses this)
- [ ] sys_set_thread_area / arch_prctl (PR_SET_TSC, PR_SET_UNALIGN)

---

## Signal Handling

fork works, exec works, but caught signals (^C, ^Z) are broken.

- [x] Re-enable sigreturn trampoline (map_sig_trampoline commented out in 3 places)
- [x] Fix deliver_pending_signals() — has `return;` before the signal loop
- [x] Test caught signals end-to-end

---

## /proc Filesystem

Mounted but mostly empty. Many tools (ps, top, free, w, uptime) read /proc.

- [ ] /proc/[pid]/status, cmdline, environ, maps, fd/, exe, comm, stat, limits
- [ ] /proc/self symlink to current process
- [ ] /proc/meminfo (from PMM stats)
- [ ] /proc/cpuinfo (from CPUID)
- [ ] /proc/version (UTS data)
- [ ] /proc/uptime, /proc/stat
- [ ] /proc/net/tcp, /proc/net/udp, /proc/net/arp (from kernel tables)
- [ ] /proc/sys/ (read-only)

---

## File Locking

Used by lock files, databases, editors.

- [ ] fcntl F_GETLK / F_SETLK / F_SETLKW

---

## Memory Syscalls

Used by malloc implementations and mmap-backed apps.

- [ ] mremap (glibc malloc, memory allocators)
- [ ] madvise (MADV_DONTNEED, MADV_WILLNEED, MADV_DONTFORK)
- [ ] msync (MS_SYNC, MS_ASYNC)

---

## Event Mechanisms

Modern event-driven daemons need these.

- [ ] signalfd / eventfd / timerfd / timerfd_create
- [ ] POSIX timers (timer_create, timer_settime, timer_gettime)

---

## ptrace

Needed by gdb, strace, any debugger.

- [ ] sys_ptrace (PTRACE_ATTACH, PTRACE_SYSCALL, PTRACE_CONT, PTRACE_PEEKDATA)
- [ ] Cross-process memory read/write
- [ ] PTRACE_SINGLESTEP

---

## Filesystem Mounts

VFS+tarfs is read-only initramfs. Need writable and disk-backed.

- [ ] Writable root filesystem (ext2 read-write, not just tarfs+COW)
- [x] UNIX domain sockets (AF_UNIX stream bind/listen/accept/connect + dgram, S_IFSOCK files)
- [ ] Mount ext2/3/4 from disk (not just initramfs)
- [ ] bind mounts, overlayfs

---

## Networking Hardening

ARP/IPv4/ICMP/UDP/TCP/DNS/DHCP basics work. Need production quality.

- [ ] TCP proper teardown (FIN/ACK, TIME_WAIT, connection recycling)
- [ ] TCP window scaling, congestion control
- [ ] TCP out-of-order reassembly
- [ ] Handle RST packets correctly
- [ ] DHCP lease renewal
- [ ] DNS resolver caching
- [ ] SOCK_NONBLOCK / SOCK_CLOEXEC flags for socket()
- [ ] epoll_create/epoll_ctl/epoll_wait (eventfd-based fallback)

---

## Security & Process Model

Everything runs as root now. Need basic isolation.

- [ ] UID/GID enforcement on file access
- [ ] setuid/setgid bits
- [ ] chmod/chown actually enforce permissions
- [ ] ASLR (randomize mmap_base, stack, vdso address)
- [ ] Kernel stack guard pages (overflow detection)

---

## Basis (basisd) Prerequisites

Do these before writing basis-core/svcmgr. Each step is kernel or initramfs work, not the supervisor itself.

- [x] Step 1 — Concurrent children: `wait4(WNOHANG)` poll + `rusage` zero-fill, `nanosleep`/`clock_nanosleep` `rem` handling, `sched_yield` stub, PIT-tick `CLOCK_MONOTONIC` (verified). NOTE: deferred run-to-completion model still serializes children — true parallel daemons need a preemptive scheduler (future work); Basis phase 1 starts services sequentially
- [x] Step 2 — Minimal identity: `uid/euid/gid/egid` in PCB, wire `GETUID/GETEUID/GETGID/GETEGID` + `SETUID/SETGID/SETREUID/SETREGID/SETRESUID/GETRESUID` (POSIX root/unprivileged rules, no saved-IDs yet) + `AT_UID/EUID/GID/EGID` auxv, fork-inherit, execve-preserve (verified: clean build + boot to `/ #` prompt, no regression)
- [x] Step 3 — Service I/O: `pipe` + `dup2` stdout/stderr capture per service to `/tmp/basis/<svc>.log` (verified live: `echo X > /tmp/svc.log 2>&1` + `cat` round-trips, `echo PIPE-CHECK | cat` works)
- [x] Step 4 — `basisctl` transport: `AF_UNIX` stream `bind/listen/accept/connect` on `/run/basis.sock` (verified live: `/bin/unix_test` 20/20 PASS — fixed `read`/`write` on raw socket fds in `vfs_read`/`vfs_write`, which previously returned raw `-1`/EPERM)
- [x] Step 5 — Lifecycle signals: `SIGCHLD` pending on parent at child exit (default-ignored, catchable), `kill` POSIX permission checks (root or uid-match, `EPERM` otherwise; self-signals unaffected), working `SYS_REBOOT (169)` with magic validation (`RESTART` → i8042 pulse with IBF drain, `POWER_OFF` → ACPI/halt; verified live: `reboot -f` resets guest). NOTE: pre-existing `sig_test` `sigpending` FAIL + GPF reproduced on unmodified baseline — out of scope, needs separate diagnosis; plain `reboot` (no `-f`) signals PID 1 which can't preempt a blocked `read` (scheduler gap, see Step 1)
- [x] Step 6 — Visibility: `/proc/[pid]/cmdline`, `/proc/[pid]/stat`, `/proc/self` symlink (already implemented; verified live: `cat /proc/1/cmdline` → `sh`, `ls /proc` lists pid dirs + `self`; fixed `stat` printing hardcoded pid `0` → now prints real pid, verified `1 (sh)`)
- [x] Step 7 — Boot wiring: `/etc/bootsh` single-path override (checked before `/etc/shells`, then `/bin/sh`) for the future `/usr/basisd/sbin/basis-core`, plus failure handoff contract (exec failure → `tui_recovery_menu(reason)` with the missing path; verified live both ways: normal boot to `/ #`, missing target → recovery TUI banner). NOTE: kernel-cmdline `bootsh=` needs the Limine cmdline request (not in `limine.h` yet) — deferred

---

## Current Focus — eigen-recovery (nmtui language) + compat syscalls

- [x] `SYS_pread64 (17)` / `SYS_pwrite64 (18)` with save/seek/read/restore + `vfs_is_seekable` (pipes/sockets/ttys/ptys → ESPIPE instead of blocking). Verified live: pread_test 5/5 PASS, `0x11` spam gone
- [x] PTY ioctls: `TIOCGPTN` / `TIOCSPTLCK` + per-pair termios/winsize routing in `SYS_IOCTL` (fixes unlock → slave-open → both data directions). pty_test re-run pending (serial RX dead in sandbox; verify via VNC or sendkey)
- [x] Lattice terminal backend: cursor tracking (no re-addressing sequential cells) + CP437→UTF-8 glyph translation (correct on Eigen + Linux)
- [x] eigen-recovery middle-box restyle: blue screen, floating white panel + shadow, title/health, telemetry, breadcrumb, menu + live status column, shell peek, glyph footer, `<buttons>`, dim-behind modals
- [x] Dirty discipline (IRQ total no longer repaints) + present throttle (keys instant, timer redraws ~5Hz)
- [ ] VNC visual pass on eigen-recovery (all screens + modals + palette)
- [ ] pty_test 17/17 PASS on metal

---

## Parked (not today)

- Lattice L4 layout algebra, L5 screen-graph, L6 manifests (L0–L2 done and proven)
- GPU stack: Linux-compatible DRM/KMS core + Bochs (QEMU) + VMware SVGA II (VirtualBox VMSVGA) + virtio-gpu backends, Intel PCI probe for real HW. QEMU exposes no Intel GPU, so Intel modeset stays a stub until hardware exists
- OpenGL: needs Mesa port; path is virgl (virtio-gpu) or softpipe/llvmpipe on dumb buffers — after the DRM core lands
