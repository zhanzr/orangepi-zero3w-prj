# CO5300 MD196 368x448 AMOLED (CHSC6417 touch) on the JP4 40-pin header

Wiring plan for the `co5300_md196_368x448_chsc6417` module (TK0196M106:
**CO5300** 1.96" 368x448 AMOLED + **CHSC6417** capacitive touch) on the
Orange Pi Zero 3W **JP4** 40-pin extension connector. Reference ports:
`D:\stm32l4-prj\nucleo-l4r5\bare\co5300_md196_368x448_chsc6417` and the
same-named `nucleo-u575` project (identical module wiring) - single-lane
SPI, wrapped-command protocol, touch @ I2C 0x2E.

## Pin mapping (verified against live DT, see below)

| Module signal | JP4 pin | SoC pin | Interface on the board | Notes |
|---|---|---|---|---|
| **CS** (QSPI_CS) | **24** | PE0 | SPI3_CS0 -> `/dev/spidev3.0` | hw CS via overlay; soft-CS fallback below |
| **CLK** (QSPI_CLK) | **23** | PE1 | SPI3_CLK | `spi3_pin_default` group |
| **MOSI** (QSPI_IO0) | **19** | PE2 | SPI3_MOSI | module is write-only; MISO **PE3 / JP4.21 stays unconnected** |
| **TOUCH_SCL** | **5** | PB2 | TWI0_SCL -> `/dev/i2c-0` | |
| **TOUCH_SDA** | **3** | PB3 | TWI0_SDA | CHSC6417 @ 7-bit **0x2E**, data from reg 0x00 |
| VCC | 1 (or 17) | V33 | 3.3 V rail | same rail the Nucleo reference used; confirm the module's tolerance before first power-up |
| GND | 9 (or any GND pin) | GND | - | common ground required |
| RST / BL / DC | - | - | **not wired** | per the reference: POR settle in software, brightness via DCS `51h`, wrapped protocol needs no DC |

Soft-CS alternative: if the hardware CS frame timing shifts the wrapped
stream (the STM32 reference needed explicit CS setup/hold margins; see
its README "The missing piece was CS setup/hold time"), rewire CS to a
plain GPIO - **PD0 / JP4.22** (adjacent to the SPI pins) or **PE11 /
JP4.15**, **PB6 / JP4.35** - and drive it via the GPIO uAPI
(`/dev/gpiochip0`, libgpiod is installed), replicating the reference's
software-CS approach. Note **TWI2/TWI3 cannot be used** for touch:
their pins (PE1/PE2/PE3/PE4) are the SPI3 pins.

## Enablement (done on the board, verified working 2026-11-06)

`/boot/armbianEnv.txt`:

```
overlays=spi3-cs0-cs1-spidev i2c0
```

then reboot. Verified after reboot:

- `/dev/spidev3.0` + `/dev/spidev3.1` exist (SPI3 CS0 = PE0, CS1 = PE4)
- `/proc/device-tree/.../spi@2543000/status` = `okay`
- **`i2c-0: SUNXI TWI(0x2510000)`** appeared = TWI0 on PB2/PB3
- overlay files used (Armbian convention - entries are written **without**
  the `overlay_prefix`): `/boot/dtb/allwinner/overlay/sun60i-a733-spi3-cs0-cs1-spidev.dtbo`,
  `sun60i-a733-i2c0.dtbo`

DT facts checked on the live tree (`/sys/firmware/fdt` via fdtget):

- `spi3_pin_default`: pins PE1 PE2 PE3 PE4 PE0, function `spi3` (+ a
  `spi3_pin_sleep` gpio_in group)
- `twi0@0`: pins PB2 PB3, function `twi0`
- spidev children `spidev@0`/`spidev@1` ship in the base DT
  (`rohm,dh2228fv` placeholder compatible); child `spi-max-frequency`
  = 100 MHz (DT cap - the panel's real limit is lower, see below)

Backup of the pre-change file: `/boot/armbianEnv.txt.bak` on the board.

## Bring-up notes (from the reference ports)

- Protocol: every transaction is ONE CS frame: `02 00 <cmd> 00` header +
  payload; SPI **mode 3**, MSB first, write-only. Pixel bursts: window
  (2Ah/2Bh) once, first row `2Ch`, further rows `3Ch`.
- Vendor init (CO5300, verbatim): `FE 00`, sleep-out `11` + 120 ms, `FE 00`,
  `C4 80`, `3A 55` (RGB565), `53 20`, `63 FF`, `51 <brightness>`, `58 07`,
  `29` + 80 ms. **Do not send software reset `01h` outside the datasheet
  rule** (it breaks the panel on the reference; POR settle + no reset pin
  wired).
- Brightness: `51h` only, linear DBV/256; the reference runs `0x01`
  (0.78 %) as the usable floor - start there.
- Speed: panel write limit ~47 MHz; 60 MHz corrupted the checkerboard on
  the reference. Start at **8-10 MHz** on spidev and climb; sunxi SPI
  keeps CS inside the kernel driver, so watch for shifted frames.
- Geometry: 368x448 RGB565, X offset 0 on the reference (vendor demo
  drives 0..367); `LCD_X_OFFSET=16` is the documented fix if shifted.
- Touch: read reg 0x00: byte0 = {count[1:0], X bit9<<6, Y bit9<<7},
  byte1 = X[7:0], byte2 = Y[7:0]; chip id at 0xA7; Y axis reports
  448-Y in the reference console.

## Software route

First bring-up app: i2c-dev for touch (`/dev/i2c-0`) + spidev
(`/dev/spidev3.0`) from userspace - same pattern as the ssd1317 OLED
app in `opz-dev` (GPIO uAPI for the optional soft-CS). A kernel touch
driver (CHSC6417 has no mainline driver; vendor touches are
non-standard) is not planned - userspace polling is enough here.
