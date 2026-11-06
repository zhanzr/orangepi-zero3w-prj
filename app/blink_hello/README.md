# blink_hello

Blinks the single on-board LED via the kernel LED class API and samples
both SoC die temperatures (Allwinner THS) on every toggle:

| Item | sysfs path |
|---|---|
| LED | `/sys/class/leds/status_led` (gpio-leds, high active) |
| big cluster temp (2x A76) | `/sys/class/thermal/thermal_zone0` (`cpub_thermal_zone`) |
| little cluster temp (6x A55) | `/sys/class/thermal/thermal_zone3` (`cpul_thermal_zone`) |

500 ms per phase, Ctrl-C to stop - the current LED trigger (the board
boots with `heartbeat`) is saved on start and restored on exit.

The LED sysfs nodes are root-owned (0644): run via `make run` (we ssh
in as `root`, so it just works) or prefix with `sudo` otherwise.

```
make push run
```

Expected output:

```
led ON   cpub (A76x2): 47.9 C   cpul (A55x6): 49.2 C
led OFF  cpub (A76x2): 47.8 C   cpul (A55x6): 49.1 C
...
```

Die temp sources: `/sys/class/thermal/thermal_zone{0,3}`, millidegrees
C (ddr/npu/gpu zones are 1/2/4 if ever needed).
