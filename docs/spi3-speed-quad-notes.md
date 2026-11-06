# SPI3 on JP4: speed ceiling and QSPI/Dual-SPI feasibility

Investigation results for `app/co5300_md196_368x448_chsc6417/lcm_test`
(verified empirically on the board, 2026-11-06; vendor kernel
`6.6.98-vendor-sun60iw2`, driver `sunxi-spi-ng`, spidev on CS0).

## 1. How fast does SPI3 actually go?

The `-s kHz` knob sets the spidev max speed; the sunxi driver then sets
the SPI3 module clock (`clk spi3`, parent `pll-peri0-200m` @ 200 MHz).
Measured facts:

- The module clock **cannot leave 40 MHz**: `clk_set_rate` (tested both
  from spidev requests and via debugfs) rounds every request of 50 /
  60 / 80 / 100 / 200 MHz down to exactly 40,000,000 Hz. Requests below
  40 do not move it either (24/30/36 MHz requests stay 40) - the
  driver pins the rate once the bus is up.
- The PLL node itself (`pll-peri0-200m`) refuses re-clocking (300/480
  MHz writes are no-ops) - the `-200m/-300m/-480m` nodes are fixed
  post-divider outputs of one PLL, so the parent rate is not raiseable
  either.
- Wire-rate ladder (1000 x 740 B CS frames timed on the board, syscall
  overhead 19 us/call subtracted; `spi_rate_probe` method):

| `-s` request | wire (measured) |
|---|---|
| 10 MHz | 9.3 Mbit/s |
| 20 MHz | 17.7 Mbit/s |
| 30 MHz | 22.3 Mbit/s |
| **40 MHz** | **29.0 Mbit/s** |
| 50 MHz | 29.7 Mbit/s (clamped, no real gain) |

**Conclusion: `-s 40000` already delivers the practical maximum
(~29-30 Mbit/s wire).** Going higher needs kernel work, not a DTB
tweak: the ceiling is inside the vendor CCU/driver (spi3 mod clock is
pinned at 40 MHz; a DTB `assigned-clock-rates` would go through the
same refused `clk_set_rate`). The CO5300 datasheet allows ~47 MHz
writes - the SoC side, not the panel, is the limit here.

For reference, full-frame throughput at `-s 40000`: 368x448 RGB565
frame = 330 KB -> ~117-134 ms -> ~8 fps (syscall-per-row dominated).

## 2. QSPI / Dual SPI for the CO5300

The module supports them (vendor quad pixel-burst opcode 32h / dual
forms; see the nucleo-l4r5 README) - but the Orange Pi Zero 3W cannot
use them today:

1. **JP4 physically routes only 2 data lines** for SPI3: IO0 = MOSI
   (PE2), IO1 = MISO (PE3). Quad needs IO0-IO3; there is no third/fourth
   data pin on the header (PE4 is CS1). So at most **Dual SPI** is
   pin-feasible; QSPI is physically impossible on JP4.
2. **The DT defines single-lane pinmux only** (`spi3_pin_default`,
   5 pins CLK/MOSI/MISO/CS1/CS0; no dual/quad groups; same for spi0's
   PC group and every other controller node).
3. **The vendor driver has no multi-lane data path**: no dual/quad
   symbols in the kernel (`spi_nor_*` hits are the flash subsystem,
   unrelated), and spidev's UAPI is single-lane by design (dual/quad
   would need driver-side lane packing per `spi-tx-nbits`/
   `spi-rx-nbits`, which sunxi-spi-ng does not implement).
4. The SoC does have a dedicated **SPIF** (SPI-flash) controller -
   the likely quad-capable block - but its pins go to the boot-flash
   footprint, not to JP4.

Bottom line: single-lane only on this image/header. Enabling Dual SPI
would take a vendor-driver extension (lane packing + a DT pinmux/mode
change, `sunxi,spi-bus-mode = <0x01>` is already present in spi3's
node - meaning dual mode is at least anticipated by the BSP - but
nothing in the shipped kernel services it). Quad would additionally
require header pins that do not exist. Given the ~30 Mbit/s single-
lane ceiling, dual SPI would be the real bandwidth unlock (~2x), but
it is a kernel development project, not a configuration change.

## Method notes

- `spi_rate_probe.c` (timing method above) is a throwaway; the probe
  source lives in the session staging dir, regenerate from this doc if
  needed.
- Clock facts: `clk_summary`/`/sys/kernel/debug/clk/spi3/*`
  (`clk_possible_parents`: sys24M, pll-peri0/1-300m/200m/480m, dcxo;
  current parent pll-peri0-200m), `dtc -I dtb -O dts /sys/firmware/fdt`
  for the live DT (spi3 `clock-frequency = <100000000>` is the DT's
  declared value, ignored in practice - the driver pins 40 MHz).
