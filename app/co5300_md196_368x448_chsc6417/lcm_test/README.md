# lcm_test - CO5300 1.96" 368x448 AMOLED + CHSC6417 touch bring-up demo
# (Orange Pi Zero 3W)

Ported from the nucleo-l4r5 / nucleo-u575 `co5300_md196_368x448_chsc6417`
references (wrapped-command protocol, vendor TK499 init verbatim) with the
demo flow of `rk3506-dev/app/st7365_md350_320x480/lcm_test` (banner -> info
-> inverted info -> TEST_STAND timed screens -> HSV gradient -> boot logo ->
scaled fonts, fps band + touch).

Wiring and the DT-overlay enablement (already applied + verified): see
`../../docs/co5300_md196_jp4-pinout.md`.

| Signal | JP4 pin | SoC pin | device |
|---|---|---|---|
| CS | 24 | PE0 | `/dev/spidev3.0` (hw CS) |
| CLK | 23 | PE1 | SPI3_CLK |
| MOSI | 19 | PE2 | SPI3_MOSI (write-only, no MISO wired) |
| TOUCH_SCL / SDA | 5 / 3 | PB2 / PB3 | `/dev/i2c-0`, CHSC6417 @ 0x2E |

SPI **mode 3**, wrapped commands: one CS frame per transaction, header
`02 00 <cmd> 00` + payload inside the same frame (one `write()`; a CS
toggle mid-payload would corrupt the stream - this is THE difference to
the rk3506 DC-panel transport). Rows stream as 2C then 3C continue
frames, one row (736 B payload) per CS frame.

## Speed: `-s 40000` is the practical maximum (verified)

- `-s speed_kHz` (default **40000**). The CO5300 family allows ~47 MHz
  writes; 60 MHz corrupted the checkerboard on the STM32 references.
- **The vendor driver cannot go faster**: requests above 40 MHz clamp
  (50/60/80/100 MHz requests all serve the same wire rate), and the
  SPI3 module clock is pinned at 40 MHz by the CCU - every
  `clk_set_rate` >= 50 MHz rounds back to exactly 40 MHz (debugfs
  verified; the parent PLL node refuses re-clocking too). A DTB change
  cannot fix this - it goes through the same clock framework. Details
  and the measured wire-rate ladder (9.3 / 17.7 / 22.3 / 29.0 /
  29.7 Mbit/s for -s 10/20/30/40/50 MHz): `../../docs/spi3-speed-quad-notes.md`.
- Measured at `-s 40000`: full-screen writes 108-134 ms (~7.5 fps),
  gradient animation **8 fps** (368x448 RGB565, syscall-per-row); wire
  rate ~29 Mbit/s (timing probe, overhead-corrected).
- QSPI/Dual SPI: the module supports them but **JP4 routes only 2 data
  lines** and the vendor driver/spidev are single-lane - see
  `../../docs/spi3-speed-quad-notes.md`.

## Text: DejaVu Sans via FreeType (the native Linux font stack)

The board ships `libfreetype.so.6` + `/usr/share/fonts/truetype/
dejavu/` - the demo renders all text (banner 56 px Bold, info page
26/30 px, fps readout 20 px, logo caption 26 px) through FreeType with
anti-aliased blending into the RGB565 line buffers (`ft_text.c`). The
hardcoded 8x16 raster font remains only as an automatic fallback if
FreeType cannot start. The `DejaVu font sizes` screen shows 16/24/32/
48/64 px on one page.

Cross builds link the arm64 FreeType from WSL multiarch
(`libfreetype-dev:arm64`, installed 2026-11-06; ports.ubuntu.com added
as an arm64-only apt source). Native on-board builds work too
(`make deploy build-remote` installs `libfreetype6-dev` on the board
first).

## Logo

The Rockchip-era boot logo is replaced by the **Orange Pi pinwheel**
from the vendor user manual cover (`ref/logo_opi.h`, 148x160 RGB565,
white margins flood-filled away and flattened onto black for the OLED)
+ a "Orange Pi Zero 3W" caption. Regenerate: `pdfimages -png` the
manual PDF, flood-fill white -> transparent from the corners, flatten
on black, convert rows to `0xXXXX` RGB565 (the one-off pipeline is in
the session notes; `ref/logo_opi.h` is committed, so this is only
needed if the vendor art changes).

## Run

```
make push run            # loops forever, Ctrl-C stops
make run-s SPEED=40000   # 60 s single run at an explicit speed
```

Console (verified on board):

```
CO5300 368x448 (MD196) + CHSC6417, wrapped SPI mode 3 (hw SPI3):
CLK=JP4.23 MOSI=JP4.19 CS=JP4.24 TOUCH=JP4.5/JP4.3 (i2c-0 0x2E)
[TOUCH] self-test: ACK (chip present) (chip id 0xA7: 0x01)
spi3: /dev/spidev3.0 mode 3 @ 40000000 Hz requested (driver may clamp)
[LCD] loop 1: brightness 0x01 (0%)
[LCD] phase: HARDWARE banner
[TOUCH] down X=161 Y=114 (448-Y=334)
[TOUCH] release
[LCD] running patterns on HARDWARE SPI3 @ 40 MHz
...
  frame : 116 ms (hold 4 s)
  gradient: 9 frames in 1059 ms = 8 fps
```

Brightness: no BL pin on an OLED - the loop alternates DCS 51h between
`0x01` (0.78 %, the l4r5 port's usable floor - the panel reads very
bright) and `0x28` (15.6 %), the analog of the LCD demo's 11 %/55 %
backlight cycle. 51h is the only brightness control (CMD2 pages
undocumented).

Touch: reg 0x00 read per poll (20 ms): byte0 = {count[2:0], X9<<6,
Y9<<7}, bytes 1/2 = X/Y low; count 0 = released, 1..5 = contacts.
Console prints the same `[TOUCH] down X=.. Y=.. (448-Y=..)` format as
the STM32 references.

Touch debugging history (for the record - what finally made it work):

1. This firmware freezes the report block at the last position on
   release instead of zeroing it, and older broken builds wedged it
   into reporting a stale "down" forever - a reboot + correct reads
   cleared that.
2. Register access is restricted: pointers must be 16-byte-aligned
   block bases, block reads <= 32 bytes; anything else (0x23, 36-byte
   reads) returns EINVAL.
3. The IC NACKs reads raced against its internal scan cycle - reads
   retry (4x, 3 ms apart).
4. The blocking bug: I2C_RDWR's ioctl **returns the number of messages
   transferred (2) on success, never zero** - a success check of
   `== 0` rejects every good transfer. `blk_read()` now treats
   `transferred == nmsgs` as success.
5. Robustness: every sample is read twice and rejected if the first
   4 bytes differ (mid-update report); main.c fires after 3 consistent
   down-samples and releases after 6 consistent up-samples (60/120 ms).

Bring-up cautions (inherited from the references):

- No reset pin: **power-cycle the module** before judging a fix; the
  CO5300 soft reset 01h is deliberately not used (datasheet 120 ms
  rule - see `lcd.c` LCD_RESET).
- If the image is shifted, rebuild with `-DLCD_X_OFFSET=16`.
- Cold power-on needs the 500 ms settle before the first clock
  (`LCD_POWER_SETTLE_MS`), else init is lost.
- `pkill -x lcm_test` before `make push` (ETXTBSY).
