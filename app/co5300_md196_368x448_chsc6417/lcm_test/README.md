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

## Speed: 40 MHz is the ceiling (verified)

- `-s speed_kHz` (default **40000**). The CO5300 family allows ~47 MHz
  writes; 60 MHz corrupted the checkerboard on the STM32 references.
- **Requests above 40 MHz are clamped by the vendor driver**: 50/60/80/100
  MHz requests all come back 40 MHz (spidev echoes the actual rate; the
  sunxi-spi-ng master max = its 40 MHz bus clock, confirmed in
  `/sys/kernel/debug/clk/clk_summary`). Exceeding it would mean re-
  clocking the bus in the DT/kernel - not attempted; 40 MHz is also the
  reference-proven rate.
- Measured at 40 MHz (SCK verified 40,000,000 Hz, 0 transfer errors):
  full-screen writes 108-134 ms (~7.5 fps), gradient animation
  **8 fps** (368x448 RGB565, syscall-per-row).

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

Touch: reg 0x00 read per poll (20 ms): byte0 = {count[1:0], X9<<6,
Y9<<7}, bytes 1/2 = X/Y low; console prints the same
`[TOUCH] down X=.. Y=.. (448-Y=..)` format as the STM32 references.
Chip id 0xA7 reads 0x01 on this module.

Bring-up cautions (inherited from the references):

- No reset pin: **power-cycle the module** before judging a fix; the
  CO5300 soft reset 01h is deliberately not used (datasheet 120 ms
  rule - see `lcd.c` LCD_RESET).
- If the image is shifted, rebuild with `-DLCD_X_OFFSET=16`.
- Cold power-on needs the 500 ms settle before the first clock
  (`LCD_POWER_SETTLE_MS`), else init is lost.
- `pkill -x lcm_test` before `make push` (ETXTBSY).
