# Lattice

A data-bound TUI toolkit in freestanding C11. Widgets bind to live sources,
not static strings; screens are headed for declarative manifests, not
hand-written render loops. Extracted from
[eigen-experiment](https://github.com/ItsSidney/eigen-experiment); it
builds for bare metal (no heap needed) and hosted Linux alike.

## The four decisions that make it Lattice

1. **Data-bound widgets (L2).** A log pane holds a ring-buffer handle plus
   a generation counter and re-renders when the counter moves. A service
   table subscribes to state-change events. Nothing copies strings around.
2. **Layout algebra (L4, in progress).** `stack / split / overlay / grid`
   compose every screen from one chrome fragment — including dim-behind
   modals that float over live content instead of blanking it.
3. **Navigation as screen graph (L5, in progress).** Menu, wizard,
   free-pane, dashboard and modal are one model, so any node can push a
   modal over itself (confirm-before-restart *on* the dashboard).
4. **Declarative screens (L6, in progress).** Screens as manifests;
   only truly custom widgets get hand-written code.

## Layers (done → planned)

| Layer | Dir | State |
|---|---|---|
| L0 surfaces — cell grid + backend vtable (framebuffer / terminal / headless-serial) | `l0/` | done |
| L1 compositor — dirty-rect diffing, z-order layers, dim-behind | `l1/` | done |
| L2 reactive — `Signal` / `Stream` / `Table` pub/sub, heap or arena-only (the core never allocates) | `l2/`, `core/` (arena) | done |
| L3 widgets — chrome, menus, inputs, tables, log panes, gauges, dashboards | — | next |
| L4 layout algebra | — | planned |
| L5 screen graph | — | planned |
| L6 manifests | — | planned |

Plus `cli/` — a whiptail-style dialog tool built on the same core
(`--menu --msgbox --yesno --inputbox --textbox --gauge`), and `tests/`.

## Build & test

```sh
make            # host static lib + lattice CLI
make test       # 69 core checks + scripted dialog flows
```

The core is freestanding (`stdint/stddef/stdbool` only): the same files
compile with `-ffreestanding -fno-builtin` into kernels. The terminal
backend needs POSIX (`termios`, `ioctl`, `poll`); the framebuffer backend
is a ~30-line adapter over `put_cell`.

```sh
choice=$(lattice --menu "Pick one" 0 0 0 a A b B 3>&1 1>&2 2>&3)
```

UI on stdout, result on stderr, exit `0` ok / `1` cancel — the dialog
convention, so existing whiptail call sites port by renaming the binary.

## License

© 2023–2026 Eldrick Sidney. All rights reserved — source-visible, not
open-source (yet). Talk to the author about licensing.
