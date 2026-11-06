# AGENT.md — guidance for AI agents working in this repo

Orange Pi Zero 3W (Allwinner A733, arm64, Armbian vendor kernel
`6.6.98-vendor-sun60iw2`) application repo. Connectivity: WiFi only
(`wlan0`, no Ethernet). LED: single `status_led` (gpio-leds, boots with
`heartbeat`). Peripherals on the JP4 40-pin header: TWI0-3, SPI3,
UART2/6/7, PWM0_0..9. Two 22-pin MIPI CSI camera connectors; no HDMI.

## Golden rules

1. **Never commit/push without the user's explicit approval.** The user
   verifies results on real hardware first. This rule has priority over
   everything else.
2. **Verify empirically before claiming anything.** Assertions without
   an on-board test are worthless (e.g. the "8x A55" guess - lscpu
   showed 2x A76 + 6x A55).
3. **Use rsync for transfers, never scp.** `rsync -az <src> opz3w:<dir>/`
   - all app Makefiles already do this.
4. Complex commands must go through **script files**: stage under
   `C:\Users\user1\AppData\Local\Temp\opencode\` and pipe with
   `cmd /c "ssh opz3w ""bash -s"" < <script>"` (or the equivalent in
   WSL). Inline quoting gets mangled by the Windows PowerShell layer -
   never pass `$`, nested quotes or heredocs through `ssh opz3w "..."`
   or `wsl.exe -e bash -c "..."` directly from PowerShell.

## Board access

- `ssh opz3w` from Windows and from WSL - both resolve the mDNS name
  `orangepizero3w.local` (WSL: mirrored networking mode +
  avahi-daemon inside WSL; see the root README). User **root**, key
  auth (`id_rsa_pocket`), no sudo needed on the board.
- The board is WiFi-only. If mDNS resolution breaks, run
  `tools/find_board.sh` from WSL - it scans the /24 and rewrites the
  `opz3w` alias with the numeric IP as a fallback.
- Long ssh sessions can wedge silently on this WiFi link (seen once
  after a ~6 min benchmark: the remote command finished, the local
  ssh never noticed). The `opz3w` ssh config sets
  `ServerAliveInterval 15` to detect dead connections; if a session
  still wedges, kill the local `ssh`/`make` and trust the on-board
  artifacts (`results.csv` etc.) as the source of truth.

## DT overlays (JP4 header peripherals)

- Peripherals are enabled with Armbian overlays in
  `/boot/armbianEnv.txt`: `overlays=spi3-cs0-cs1-spidev i2c0` applied
  (2026-11-06) - `/dev/spidev3.0` + `i2c-0` (TWI0, PB2/PB3) verified
  after reboot. Overlay files:
  `/boot/dtb/allwinner/overlay/sun60i-a733-*.dtbo`; **entries in
  `overlays=` are written WITHOUT the `overlay_prefix`** (the boot
  script prepends `sun60i-a733-` - full filenames silently do not
  apply). Available overlays include i2c0-3, pwm0-9, uart2/6, lcd,
  spi3-cs0-cs1-spidev.
- A backup of the pre-overlay file is on the board:
  `/boot/armbianEnv.txt.bak`.
- `fdtget`/`fdtput`/`dtc` + libgpiod (`gpioinfo`) are installed on the
  board; the live DT is readable at `/sys/firmware/fdt`.

## Layout

- `app/<project>/` - one self-contained directory per app (Makefile +
  README + sources).
- `docs/` - notes and pointers; vendor manuals stay outside the repo.
- `tools/` - `find_board.sh` (IP rescan), udev rules / dts snippets as
  needed.
- `board_images/` - board photos (moved here from
  `D:\board_database\orangepi-zeo3w\board_images`).

## Build & deploy

- Cross-build in WSL: `make` then `make push` - rsync into
  `~/<project>/` on the board. Toolchain: `aarch64-linux-gnu-gcc`
  (apt `gcc-aarch64-linux-gnu`), `-march=armv8-a` (A76/A55; do not
  assume armv7 flags).
- Native build on the board works too: `make deploy` + `make
  build-remote` (board has gcc 15 / make / rsync / python3).
- Overwriting a running binary fails with **ETXTBSY** - `make stop`
  (`pkill -x <app>`) before `make push` if the app is still running.

## LED conventions

- `/sys/class/leds/status_led`, gpio-leds driver, `max_brightness` 1,
  high active; boots with the `heartbeat` trigger.
- Apps that toggle brightness must save the current trigger first and
  restore it on exit (including SIGINT/SIGTERM) - see
  `app/blink_hello/blink_hello.c`.
- Writes need root - we ssh as root, so no extra setup. Revisit (udev
  rule like the old board's `tools/leds.rules`) if a non-root user is
  ever created.

## Temperature conventions

- `/sys/class/thermal/thermal_zone0` = `cpub_thermal_zone` (A76 pair),
  `thermal_zone3` = `cpul_thermal_zone` (A55 six), both Allwinner THS,
  millidegrees C. zone1 = ddr, zone2 = npu, zone4 = gpu. Use the
  cpub/cpul pair for "SoC temp".

## Benchmark conventions

- Pin clocks first: `performance` governor on all 8 cpus (A55 cluster
  max 1794 MHz, A76 cluster max 2002 MHz), restore `schedutil`
  afterwards - the app Makefiles do this (`make run-perf`; note the
  remote command must be single-quoted so the local shell does not eat
  `$g`/`$?`).
- A valid CoreMark run needs > 10 s: the A76 does ~15300 CM/s, so
  >= ~154000 iterations; defaults are set accordingly.
- Makefile recipes that ssh with embedded `$` go through make's `$$`
  first, then the local shell - single-quote the remote command.

## Commit discipline

- Batch commits after the user verifies; descriptive messages with
  measured numbers; document root causes.
- Keep binaries out of git (`.gitignore` has per-app entries).
