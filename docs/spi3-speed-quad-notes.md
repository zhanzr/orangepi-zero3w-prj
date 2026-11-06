# SPI3 on JP4: clock mapping, measured throughput, and multi-I/O feasibility

Investigation results for `app/co5300_md196_368x448_chsc6417/lcm_test`
(vendor kernel `6.6.98-vendor-sun60iw2`, driver `sunxi-spi-ng`,
compatible `allwinner,sunxi-spi-v1.3`, spidev on CS0), rechecked
2026-11-07 against the live board, the A733 and CO5300 datasheets, and
raw spidev transfers.

## 1. Speed: rates between 40 and 50 MHz are reachable

The A733 datasheet section 3.10.6 specifies a maximum SPI controller
clock of 100 MHz. This is a SoC limit, not a promise that every rate
request reaches the SCK pin or that the panel can accept it. The CO5300
datasheet's serial write timing gives a 20 ns minimum clock period
(50 MHz); it also explicitly limits the 4-wire QSPI write clock to
50 MHz for the listed RGB formats.

The live board DTB has `clock-frequency = 200000000` on SPI3. After
each transfer, the kernel clock summary reported these SPI3 module-clock
selections for representative requested rates:

| Requested rate | SPI3 module clock |
|---:|---:|
| 40 MHz | 40 MHz |
| 41-42 MHz | 40 MHz |
| 43 MHz | 42.86 MHz |
| 44-47 MHz | 43.64 MHz |
| 48-49 MHz | 48 MHz |
| 50 MHz | 50 MHz |
| 60 / 80 / 100 MHz | 60 / 80 / 100 MHz |

This disproves the earlier claim that requests of 50 MHz and above
cannot clock SPI3. A separate raw-write sweep (100 transfers of 4096 B,
single lane) measured payload throughput:

| Requested rate | Payload throughput |
|---:|---:|
| 40 MHz | 36-38 Mbit/s |
| 41-47 MHz (most values) | 24-29 Mbit/s |
| 48 MHz | 42 Mbit/s |
| 49 MHz | 25-26 Mbit/s |
| 50 MHz | 42-44 Mbit/s |
| 60 / 80 / 100 MHz | 53 / 68 / 78 Mbit/s |

Throughput includes transfer and syscall overhead, so it is a lower
bound on SCK, not a direct pin-frequency measurement. The SPI3 clock
provider rate is not itself proof of SCK either. The result is clear
enough to answer the practical question: requests at 48 and 50 MHz
produce more than 40 Mbit/s of single-lane payload, so the bus is not
clamped below 40 MHz. The SCK pin was not captured with a logic
analyzer; use one for an exact electrical frequency.

There is a non-monotonic rate-selection region: many requests from
41-47 MHz and 49 MHz give substantially lower payload throughput than
40, 48, or 50 MHz. Do not assume a larger requested rate is faster;
benchmark the exact value. `SPI_IOC_WR_MAX_SPEED_HZ` accepts/stores the
requested ceiling, and its readback is not an SCK measurement.

The CO5300's 50 MHz timing limit is the relevant panel ceiling; 60 MHz
corrupted the checkerboard on the STM32 reference boards. The 40 MHz
rate was visually verified in standard mode. The app supports only
standard mode. A raw SPI throughput test only proves the host is
clocking data, not that the display decodes it correctly.

The current board DTB uses `clock-frequency = 200000000` and persists
at `/boot/dtb/allwinner/sun60i-a733-orangepi-zero3w.dtb` (backup:
`/boot/dtb/allwinner/sun60i-a733-orangepi-zero3w.dtb.orig`).

The application uses standard mode at 40 MHz; higher settings need
panel-image verification.

An earlier application test observed a black panel at high settings.
Raw SPI transfers still clocked at these settings, so that observation
is a panel/app result, not evidence that the SPI clock was unavailable.

- The earlier display-time measurements showed a performance dip at
  many requests in this band; the raw SPI sweep above also shows it is
  not uniform (48 MHz is a fast point).
- Raw SPI tests now confirm clocked transfers at and above 50000. The
  earlier black-panel result does not mean the host clock was
  unavailable; the cause needs a visual/electrical panel test.
- The default remains 40000 because this rate is visually validated.

## 2. Dual / Quad SPI: host API support is present; wiring is the constraint

Panel side (CO5300 datasheet 5.2, verified text): the QSPI interface has
three operating modes - Standard SPI (1-wire), Dual SPI (2-wire), Quad
SPI (4-wire) - toggled at runtime by wrapped commands:

| wrapped instruction | action |
|---|---|
| 0x3B | Enter dual SPI |
| 0x38 | Enter quad SPI |
| 0xFF | Reset dual & quad SPI to single SPI |

These are interface-mode instructions, not DCS register commands:
transmit each opcode directly as one byte, without the normal
`02 00 <cmd> 00` wrapped-command header.

In Dual mode, the datasheet specifies 2-wire pixel-write instructions
`0x72`/`0x52`, with `0x72` carrying the 24-clock `0x002C00`/`0x003C00`
address. It does not specify a 2-wire RGB565 format. The app therefore
an earlier version of the app tried `0x72` dual-lane pixel frames,
converting RGB565 drawing buffers to RGB666 and setting COLMOD to
18-bit. This experiment could leave the module in an unusable state;
the Dual-mode code has been removed. The app now uses standard SPI only.

On the live kernel, `SPI_IOC_WR_MODE32` accepted `SPI_TX_DUAL` and
`SPI_TX_QUAD`. `SPI_IOC_MESSAGE` accepted `tx_nbits = 2` and `4` with
the corresponding mode flag. At a requested 40 MHz, 100 transfers of
4096 bytes measured about 36 Mbit/s single-lane, 63 Mbit/s dual-lane,
and 111 Mbit/s quad-lane. This demonstrates that the spidev/controller
path does support multi-lane throughput. A quad transfer without the
matching mode flag is rejected.

These are host-side throughput tests only. They do not prove the
module received correctly serialized pixels on every lane or that the
OLED displayed a valid image. The app no longer requests multi-lane
transfers; it uses standard SPI only.

## 2b. Historical note: the 0x23/0x53 "flags" and the wedged touch state

Early touch debugging (before the two-consecutive-read gate) observed
regs 0x23/0x53 = 0xFF during a held touch and 0x00 when idle - an
artifact of that wedged session, not a protocol feature; do not build
on it. The reliable down indicator is byte0 bit0 with the double-read
gate (see touch.c).

The host interface supports dual and quad transfer widths. The module
FPC exposes SI00, SI01, SI02, and SI03; SI00 is wired to JP4.19 (PE2)
and SI01 to JP4.21 (PE3) on this board. Dual mode was attempted by the
app, but was removed after tests left the module in an unusable state.

The A733 GPIO multiplexing table lists only SPI3 CLK, MOSI, MISO, CS0
and CS1; it does not identify SPI3 IO2/IO3 pins. Quad-width transfers
are accepted by spidev and show increased host throughput, but that
does not establish an electrical route for all four panel data pins.
Quad display operation on this board remains unverified; do not call it
fundamentally impossible based only on the SPI controller block's
datasheet feature list.

## 3. Reference: what this firmware/driver accept

- Register access on the touch IC (see `touch.c`) is a different bus
  (TWI0) - unaffected by any of the above.
- `spidev` transfers > 32 bytes: fine for the SPI controller (DMA
  handles it); the 32-byte cap is a TOUCH IC register-access rule, not
  an SPI one.
- Driver source: `drivers/spi-ng/spi-sunxi.c` in the vendor BSP
  (docs.avaota.fun documents it; public mirrors come and go - the
  AvaotaOS/R0xyLee GitHub trees held copies).
