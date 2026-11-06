# Docs

Setup and design notes for the Orange Pi Zero 3W project.

| File | Content |
|---|---|
| `co5300_md196_jp4-pinout.md` | CO5300 1.96" 368x448 AMOLED + CHSC6417 touch on JP4: full wiring (SPI3 + TWI0), DT overlays enabled + verified, bring-up notes |
| `e902-coprocessor.md` | The RISC-V XuanTie E902 (200 MHz): what it is, why it is NOT user-programmable on the vendor image, what the mainline route would take |

Vendor manuals (PDFs, schematics, tools) are kept in
`D:\board_database\orangepi-zeo3w` and are not part of this repository.
The JP4 40-pin extension connector table from the vendor README is kept
there too (serial console wiring: Ext.6 = GND, Ext.8 = UART0_TX,
Ext.10 = UART0_RX, 115200 8N1).

| Vendor file / folder | Content |
|---|---|
| `OrangePi_Zero3W_A733_用户手册_v1.0.pdf` | Vendor user manual (CN): A733 SoC details, header pinouts, image flashing |
| `原理图/` | Board schematics |
| `官方工具/` | Vendor flashing tools |
| `ai-sdk.tar.gz` | Vendor AI SDK bundle |
| `README.md` | Onboard resource notes: JP4 40-pin table (TWI0-3, SPI3, UART2/6/7, PWM0_0..9), serial console wiring, LED (PWM_LED1 / PWM0-7, high active), CPU frequency policy |
| `board_images/` (in this repo, moved from the vendor folder) | Board photos / connector close-ups |

## Armbian notes

- Image: Armbian community 26.11.0-trunk.123, `BOARD=orangepizero3w`,
  `BOARDFAMILY=sun60iw2`, `ARCH=arm64` (see `/etc/armbian-release` on
  the board).
- Kernel `6.6.98-vendor-sun60iw2` is the vendor (Allwinner SDK-based)
  kernel, not mainline. Rebuilding kernel/u-boot needs the vendor
  SDK/orangepi-build - application development uses the plain aarch64
  cross compiler.
- DT overlays for the 40-pin header (i2c/spi/uart/...): `armbian-config`
  -> System -> Hardware, or edit `/boot/armbianEnv.txt` (`overlays=`
  line), then reboot. **Not yet exercised on this board** - verify
  before relying on it.
- CPU frequency policy: clusters scale dynamically (schedutil); check
  with `cat /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor`,
  lock with `echo performance > .../scaling_governor` (sudo) - see the
  vendor README for the full recipe.
