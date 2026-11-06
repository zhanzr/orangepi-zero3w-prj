# MIPI CSI (CAM1) + OV5647 - feasibility investigation

Rechecked against the live board on 2026-11-07. **The camera hardware is
plausible and the OV5647 responds to I2C, but this image cannot capture
from it as-is.** The vendor VIN stack has no OV5647 sensor module, and
the CAM1 sensor slot still names IMX219. I2C ID reads prove the sensor
is powered and its control bus works; they do not prove CSI lane
reception, ISP operation, or V4L2 capture. There is no `csi_to_lcd`
application in this repository yet.

## What is present on the board

- CAM1 (22-pin MIPI CSI): four MIPI data lanes plus clock, TWI11_SCK/SDA
  (pins 20/21), PE6 (pin 17) and PE5 (pin 18) as camera controls, and
  `VCC_3V3_CSI` (pin 22), per the vendor manual.
- The live device tree has `vind@5800800`, CSI nodes
  `5820000/5821000/5822000`, and MIPI nodes `5810100/5810200/5810300`
  enabled. ISP `5900000` is enabled; other ISP instances are disabled.
  VINC channels 0 and 8 are enabled, while most other VINC channels are
  disabled. VINC0 selects sensor-list 0 / CSI0 / MIPI0, the CAM1 route.
- The active CAM1 slot `sensor@5812000` is configured as `imx219` on
  TWI11, with MCLK1 and a PWDN tuple for PE6. Its raw
  `sensor0_twi_addr` DT cell is `0x20`; this is not the 7-bit I2C
  address as printed by `i2cdetect`.
- **Correction to the earlier investigation:** the live DT does contain
  VINC nodes; the earlier statement that no VINC nodes exist was wrong.
  Module logs saying `vincN is null` refer to unavailable/disabled
  channel IDs. The enabled CAM1 route exists in DT, but its ability to
  produce a capture node still needs validation with a matching sensor
  driver.
- Installed VIN modules include `vin_io.ko`, `vin_v4l2.ko`, and sensor
  modules for `imx219`, `gc030a`, `gc05a2`, and `ov13850`. There is no
  OV5647 module. The standard kernel config has neither
  `CONFIG_VIDEO_OV5647` nor `CONFIG_VIDEO_IMX219`; the vendor IMX219
  driver is a separate BSP module, not the upstream V4L2 sensor driver.
- VIN modules are currently unloaded and the board currently has no
  `/dev/video*` or `/dev/media*` nodes. On an earlier manual VIN-module
  load with the OV5647 attached, the IMX219 probe failed with “chip
  found is not an target chip” / “No such device”; VIN also logged null
  for disabled VINC channel IDs and a missing `vipp16` IRQ resource.
  These observations do not establish that the enabled VINC0 route is
  unusable.
- The available `ai-sdk.tar.gz` was checked: it contains no BSP
  `drivers/vin` source tree, kernel source tree, or OV5647 driver source.
  A matching vendor BSP kernel source tree is still needed for a
  maintainable driver port.

## OV5647 control-bus verification

- With PE6 driven high and PE5 pulsed for reset, `i2cdetect -y 11`
  shows **0x36**; registers `0x300A/0x300B` return **0x56/0x47**.
  The OV5647 chip ID is therefore confirmed over TWI11.
- Observed module behavior is PE6 low = powered down and PE6 high =
  running. The current DT PWDN tuple ends in flags `0` (active-high in
  GPIO flag convention); reconcile that with the module behavior and
  the BSP driver's polarity handling before enabling the camera from
  the kernel.
- The current CAM1 DT address cell is `0x20` for an IMX219 whose
  7-bit I2C address is `0x10`, indicating a shifted-address convention
  in this BSP field. Do not copy the OV5647's 7-bit `0x36` directly
  into the DT property without confirming the framework semantics; it
  may require `0x6C`.
- To access the sensor from userspace while the VIN modules hold PE6,
  the earlier test unloaded them in dependency order:
  `rmmod imx219; rmmod vin_v4l2; rmmod vin_io`, then drove PE6 and
  read the ID.

## Feasibility and required work

**Verdict: likely feasible on the board hardware, but not plug-and-play
and not solvable with an app-only or DT-only change.** The connector
wiring, CSI receiver, MIPI blocks, and an enabled CAM1 VINC route are
present. What is unproven is a working sensor-to-VIN configuration and
actual frame capture from this OV5647.

1. Obtain the matching vendor BSP kernel source and determine the sensor
   module API used by `imx219.ko`. The AI SDK archive available here is
   not that kernel source.
2. Port or enable an OV5647 driver for that BSP API. It needs correct
   OV5647 mode/register tables and handling for its clock, PE6
   power-down, PE5 reset, and the BSP I2C address convention.
3. Configure the active CAM1 sensor slot with the driver's expected
   module name, address encoding, and power polarity. Preserve and
   validate the existing CSI0/MIPI0/VINC0 route; do not add duplicate
   VINC nodes based on the earlier mistaken diagnosis. Resolve the
   `vipp16` IRQ issue only if it blocks the selected capture path.
4. Load the sensor and VIN modules, confirm successful sensor binding,
   then verify V4L2/media nodes and capture frames. CSI data reception,
   mode timing, and ISP output are not proven by reading the I2C ID.
5. After capture works, add a `csi_to_lcd` userspace app to feed frames
   into the OLED drawing path. No such app currently exists here.

The shipped `imx219.ko` cannot drive OV5647: the sensors have different
register maps, and the BSP binds sensor modules by name from the DT
(currently `sensor0_mname = "imx219"`).

Switching to a mainline kernel is another possible route, but requires
checking that the selected A733 mainline build supports this board's CSI
receiver and camera routing; upstream OV5647 driver availability alone
does not guarantee that platform pipeline.
