# Eigen Experiment

An experimental x86-64 operating system written from scratch: freestanding
kernel, musl userspace, declarative init supervisor, a data-bound TUI
toolkit, and a Wayland stack in progress — all in one tree, all building
from a single `make`.

> Pre-alpha research system. It boots to a shell, runs busybox, serves
> its own init, and renders its own consoles. Not for daily driving. Yet.

---

## What lives here

| Area | Path | Status |
|---|---|---|
| Kernel (musl ABI, SMP, net, PTY, DRM plans) | `src/kernel` | boots, shell works |
| **Basis** — declarative service supervisor | `src/user/basis` | working init (PID 1) |
| **BasisUI** — service console | `src/user/basis/ui` | working |
| **Lattice** — reactive TUI toolkit (L0–L2) | `src/libs/lattice` | working |
| `lattice` — whiptail-style CLI on Lattice | `src/libs/lattice/cli` | working |
| **eigen-recovery** — diagnostic console | `src/user/recovery` | working |
| Wayland compositor + client (libwayland vendored) | `src/user/compositor`, `src/user/wlclient` | in progress |
| Framebuffer/mmap test tools | `src/user/fbtest`, `src/user/shmtest` | working |

Standalone mirrors: **[basis](https://github.com/ItsSidney/basis)**
supervisor on its own, **[lattice](https://github.com/ItsSidney/lattice)**
TUI toolkit on its own.

## Quickstart

Prerequisites: `gcc`, `nasm`, `xorriso`, `limine`, `qemu-system-x86_64`.
The musl cross toolchain ships in `toolchain/` — no setup needed.

```sh
make eigen.iso     # kernel + initramfs + bootable ISO
make run           # QEMU, GTK display, serial on stdio
make run-vnc       # QEMU headless + VNC on :5 (serial stays on your tty)
make run-uefi      # UEFI boot via OVMF
```

Then inside the guest:

```sh
eigen-recovery                 # diagnostic console (log, regs, hex, shell…)
basisui                        # service supervisor console
lattice --menu "Pick" 0 0 0 a A b B   # whiptail-style dialogs
onfail -- mount /data          # run it; on failure the console pops up
```

## Testing without booting

Most userspace code proves itself on the host:

```sh
make lattice-test       # Lattice core: 69 checks
make lattice-cli-test   # lattice CLI: scripted dialogs
make recovery-test      # eigen-recovery: scripted screens
make basisui-test       # basisui: fixture service tree
```

Kernel-space checks (`pty_test`, `unix_test`, …) run in-guest; see `TODO.md`.

## Layout

```
src/kernel/            GDT/IDT, MM (PMM/VMM/kheap), scheduler-less run-to-completion
                       procs, VFS (tarfs/devfs/ptsfs/tmpfs/procfs), net
                       (ARP/IPv4/ICMP/UDP/TCP/DNS/DHCP, sockets), drivers
                       (e1000, pcnet, PCI, PIT, PS/2, framebuffer, vterm),
                       TUI recovery menu, libvterm glue
src/user/basis/        basis-core (PID 1) → basis-svcmgr (manifest DAG,
                       milestones, restarts) + basisctl + basis-log +
                       basis-ntpsync + basisui console
src/libs/lattice/      L0 cell surfaces (fb/terminal/headless), L1 compositor
                       (layers, dim-behind modals), L2 reactive bindings
                       (Signal/Stream/Table, heap or arena-only)
src/user/recovery/     eigen-recovery: live dashboard, stream log viewer,
                       register grid with diffs, storage + force-mount
                       overlay, hex annotator, background pty shell,
                       command palette, postmortem export
src/user/compositor/   Wayland compositor (wl_shm → /dev/fb0)
src/user/wlclient/     sample Wayland client
initramfs_root/        guest rootfs (busybox farm, configs, manifests)
iso_root/              Limine boot tree
toolchain/             prebuilt musl (+curl/openssl toolchains for ports)
tools/                 vendored sources (tarballs tracked, trees ignored)
third_party/           libvterm, libwayland, libffi
```

## How it fits together

```
firmware → Limine → kernel → /etc/bootsh → basis-core (PID 1)
                                              └→ basis-svcmgr → milestones → /bin/sh
                                                                                 ├→ eigen-recovery
                                                                                 ├→ basisui
                                                                                 └→ lattice dialogs
```

Basis boots the machine from declarative TOML manifests; Lattice renders
every console from live data bindings instead of static strings; the
Wayland stack-in-progress will eventually replace the framebuffer shell.

## Current focus & roadmap

See [`TODO.md`](TODO.md) for the full kernel-compat checklist. Short version:

- [x] musl userspace, fork/exec, signals, PTY, AF_UNIX, tmpfs/procfs
- [x] Basis init + `basisctl` + BasisUI
- [x] Lattice L0–L2 + `lattice` CLI + eigen-recovery + onfail wiring
- [ ] Wayland compositor parity, GPU modesetting (Bochs → virtio-gpu)
- [ ] Lattice L4 layout algebra, L5 screen graph, L6 manifests
- [ ] TCP hardening, epoll, DNS caching, security model (UID/ASLR)

## License

© 2023–2026 Eldrick Sidney. All rights reserved — no license file ships
yet, so this code is source-visible, not open-source. Talk to the author
about licensing.
