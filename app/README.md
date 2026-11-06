# Applications

User-space applications for the Orange Pi Zero 3W (Allwinner A733,
arm64, Armbian 26.11). Each subdirectory is one self-contained project
with its own `Makefile` and `README.md`.

| Project | Description |
|---|---|
| `blink_hello/` | Blinks the on-board `status_led` + samples both SoC die temps (cpub A76 / cpul A55 thermal zones) |

## Common workflow (per project, from the project directory)

Fast loop during development - cross-build on the host (WSL) and push
only the binary:

```
make        # aarch64-linux-gnu-gcc
make push   # rsync the binary to ~/<project>/ on the board
make run    # execute it there
```

Deploy sources + Makefile (then build on the board with its native gcc):

```
make deploy
make build-remote
```

Requirements: the `opz3w` ssh alias in `~/.ssh/config` (see the root
README) and `aarch64-linux-gnu-gcc` on PATH (WSL apt package
`gcc-aarch64-linux-gnu`). `env.sh` at the repo root documents/exports
the variables; the Makefiles default to the same values and work
without sourcing it. Login is root, so LED/sysfs apps need no extra
permission setup on this board.
