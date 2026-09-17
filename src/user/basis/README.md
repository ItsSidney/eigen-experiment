# Basis

Declarative service supervisor: a tiny PID 1, a manifest-driven service
manager, a control CLI, and a console UI. Extracted from
[eigen-experiment](https://github.com/ItsSidney/eigen-experiment) to stand
alone — it runs on Linux too, not just Eigen.

## Pieces

```
core/      basis-core — PID 1. Reaps, restarts a flapping svcmgr a few
                       times, then falls back to a shell. Never exits.
svcmgr/    basis-svcmgr — loads TOML manifests, orders the dependency DAG,
                       walks milestones, restarts on failure with budgets
                       and backoff, keeps per-service logs + state snapshot.
ctl/       basisctl — status, deps, enable/disable, clear, apply,
                       rollback, validate, boot-until, tzset.
log/       basis-log — service log viewer.
ntp/       basis-ntpsync — clock sync service.
lib/       shared TOML parser + state-snapshot reader.
include/   shared headers (paths, manifest + state structs).
ui/        basisui — full-screen service console (needs Lattice: it is
                       fetched automatically if missing, see below).
```

## Manifests

```toml
[service]
name = "multi-user/motd"
description = "Print the multi-user sign-on banner"
type = "oneshot"              # or "daemon"
command = "/bin/busybox echo Eigen multi-user ready"
require-online = ["system/identity"]
milestone = "multi-user"
restart = "never"             # never | on-failure | always | on-abnormal
retry-budget = 1
retry-window = 60
```

Runtime layout (all under `/tmp`, which is tmpfs on Eigen):

| Path | Contents |
|---|---|
| `/etc/basis/manifests` | shipped manifests (read-only root) |
| `/tmp/basis/overlay` | applied manifests, shadow `/etc` versions |
| `/tmp/basis/override/<name>` | one-line `enabled=0\|1` admin overrides |
| `/tmp/basis/generations/<slug>/<N>.toml` | immutable applied versions |
| `/tmp/basis/maint/<slug>` | sticky maintenance marker |
| `/tmp/basis/log/<slug>.log` | per-service output |
| `/tmp/basis/state` | text snapshot for `basisctl` |

`SVC <name> <state> <since> gen=<g> exit=<e> [reason]` per line; states are
`uninit offline starting online restarting disabled maintenance`.

## Build

Needs `musl-gcc` in `PATH` (or point `MUSL_GCC` at a toolchain):

```sh
make            # builds basis-core, basis-svcmgr, basisctl, basis-log, basis-ntpsync
make ui         # + basisui console (fetches lattice if ../lattice is absent)
make test       # host-side checks
```

Install layout mirrors Eigen: `sbin/` gets the daemons, `bin/` the tools.

## basisui keys

`↑↓` select · `Enter` detail · `L` log · `D` deps · `E`/`X` enable/disable ·
`C` clear maintenance · `V` validate · `?` help · `Q`/`Esc` quit.
Destructive actions confirm first.

## License

© 2023–2026 Eldrick Sidney. All rights reserved — source-visible, not
open-source (yet). Talk to the author about licensing.
