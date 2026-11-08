# MIPI CSI: IMX219 and OV5647 bring-up

Rechecked on the live Armbian board on 2026-11-08 after a powered-off
swap and again after a later power cycle. **Both native 22-pin IMX219
modules probe, and both CAM1 and CAM2 delivered 1920x1080 YUV420M
frames through the vendor AWISPdemo. CAM2 dequeued five consecutive
frames, and CAM1 dequeued 30 consecutive frames with 29 frame sets
written to disk.** The previous CAM2 module was moved to CAM1; a
different native 22-pin module was installed on CAM2. The older CAM1
setup, which failed detection, used the previous CAM1 sensor with a
15-to-22 cable. This makes that sensor/cable combination a suspect, but
does not distinguish a bad module from cable pinout, orientation, or
seating.

Both sensors answer the vendor driver's identification check over their
respective control buses, and the Armbian VIN stack registers both
sensor/capture paths. The frame tests prove that each CSI/VIN/ISP path
can deliver frames on this Armbian image. The configured video nodes
reported 1920x1080 `YM12` with three planes; the driver allocates
2,088,960 bytes for Y and 522,240 bytes each for U and V (including
height alignment). CAM2 dequeued frames 0 through 4 in one short run.
Its 640x480 luma preview showed scene structure when contrast-stretched,
but the original luma was low and the chroma was nearly neutral. The ISP
could not find an IMX219 tuning profile and fell back to OV13850
settings, with additional warnings. Thus basic frame acquisition works,
but calibrated color/exposure are not established. The two routes were
tested serially through the shared ISP, not simultaneously. A roughly
one-second CAM1 run delivered 30 frames without a timeout; longer-term
reliability remains untested.

The vendor-reported IMX219 test is encouraging, but its image/kernel
version and exact capture procedure have not been compared with this
Armbian installation. A separate community investigation reports
failure to receive CSI packets on an Orange Pi Zero 3W despite extensive
hardware experiments; full-resolution frames from both ports on this
Armbian image are contrary evidence to a completely nonfunctional CSI
path, although image quality and simultaneous capture remain unverified.

## What is present on the board

- CAM1 (22-pin MIPI CSI): four MIPI data lanes plus clock, TWI11_SCK/SDA
  (pins 20/21), PE6 (pin 17) and PE5 (pin 18) as camera controls, and
  `VCC_3V3_CSI` (pin 22), per the vendor manual.
- The live device tree has `vind@5800800`, CSI nodes
  `5820000/5821000/5822000`, and MIPI nodes `5810100/5810200/5810300`
  enabled. ISP `5900000` is enabled; other ISP instances are disabled.
  VINC channels 0 and 8 are enabled, while most other VINC channels are
  disabled. VINC0 is configured to select sensor-list 0 / CSI0 / MIPI0,
  but `sensor_list@5812040` (sensor-list 0) itself is disabled, so this
  does not establish a usable CAM1 capture pipeline.
- The active CAM1 slot `sensor@5812000` is configured as `imx219` on
  TWI11, with MCLK1 and a PWDN tuple for PE6. Its raw
  `sensor0_twi_addr` DT cell is `0x20`; this is not the 7-bit I2C
  address as printed by `i2cdetect`.
- The live DT also has `sensor@5812020` enabled with
  `sensor2_mname = "imx219_2"`, address cell `0x20`, and MCLK2. This
  is the second IMX219 profile. Linux maps CAM1's TWI11 to `/dev/i2c-11`
  and CAM2's separate TWI9 to `/dev/i2c-9`.
- **Correction to the earlier investigation:** the live DT does contain
  VINC nodes; the earlier statement that no VINC nodes exist was wrong.
  Module logs saying `vincN is null` refer to unavailable/disabled
  channel IDs. VINC0's selection fields point to sensor-list 0 / CSI0 /
  MIPI0, but that sensor-list node is disabled; this does not establish
  an operational CAM1 route.
- Installed VIN modules include `vin_io.ko`, `vin_v4l2.ko`, and sensor
  modules for `imx219`, `gc030a`, `gc05a2`, and `ov13850`. There is no
  OV5647 module. The standard kernel config has neither
  `CONFIG_VIDEO_OV5647` nor `CONFIG_VIDEO_IMX219`; the vendor IMX219
  driver is a separate BSP module, not the upstream V4L2 sensor driver.
- On the first 2026-11-08 check, `vin_io`, `vin_v4l2`, and `imx219`
  were loaded with only one sensor reported connected. CAM1 probe
  failed, there were no `/dev/video*` nodes, and neither camera bus
  responded. The driver reported an invalid chip ID; that did not test
  CSI reception.
- On the previous check, only the CAM2 `imx219_2` profile bound; CAM1's
  older 15-to-22 sensor/cable setup did not.
- On the latest check, the previous CAM2 native 22-pin sensor was moved
  to CAM1 and a different native 22-pin IMX219 was installed on CAM2.
  The driver logged “find the sony IMX219” twice. Sysfs showed
  `/sys/bus/i2c/devices/11-0010` bound to `imx219` and
  `/sys/bus/i2c/devices/9-0010` bound to `imx219_2`.
- The media graph contained both sensor entities. `imx219` routed via
  `sunxi_mipi.0` and `sunxi_csi.0`; `imx219_2` routed via
  `sunxi_mipi.1` and `sunxi_csi.1`. Both routes reached the ISP and
  their capture nodes: CAM1 `/dev/video0` through `vin_cap.0`, CAM2
  `/dev/video8` through `vin_cap.8`. Both sensor subdevices enumerated
  `MEDIA_BUS_FMT_SRGGB10_1X10`.
- Direct V4L2 `VIDIOC_S_FMT` attempts on CAM1 previously failed with
  `Invalid argument` and VIN scaler-selection warnings. They did not
  rule out the vendor ISP-assisted capture path.
- The first AWISPdemo attempt used an interactive save path longer than
  its 15-character `scanf` limit. The remaining path text interfered
  with the following numeric prompts, and that run segfaulted; this was
  an invalid invocation, not evidence that the sensor was broken.
- Retrying with command-line arguments
  (`AWISPdemo 0 0 640 480 /tmp 1 1`) succeeded. It reported
  `VIDIOC_STREAMON ok`, dequeued frame 0, and `VIDIOC_STREAMOFF ok` in
  about 0.13 seconds. The output files were
  `/tmp/fb0_y1_640_480.bin` (307200 bytes),
  `/tmp/fb0_u1_640_480.bin` (76800 bytes), and
  `/tmp/fb0_v1_640_480.bin` (76800 bytes), the expected 640x480
  YUV420M plane sizes. `v4l2-ctl --get-fmt-video` then reported
  640x480 `YM12`, three planes, on `/dev/video0`.
- After the next user-reported power cycle, the board was reachable over
  SSH at `orangepizero3w.local` (192.168.5.100). The VIN modules were
  not auto-loaded on that boot; loading `vin_io`, `vin_v4l2`, then
  `imx219` again detected both sensors and created `/dev/video0` and
  `/dev/video8`.
- CAM2 needed its media links and RAW10 formats configured before using
  the ISP: `imx219_2 -> sunxi_mipi.1 -> sunxi_csi.1 -> sunxi_tdm_rx.0
  -> sunxi_isp.0 -> sunxi_scaler.8 -> vin_cap.8 -> /dev/video8`.
  The CAM1 route was then selected through the same shared ISP.
- `AWISPdemo 8 0 640 480 /tmp 1 1` captured one CAM2 frame and wrote
  Y/U/V files of 307200/76800/76800 bytes. A contrast-stretched luma
  preview showed scene structure; the unstretched luma values were
  33-43 and chroma values were near neutral. This confirms frame data,
  not acceptable exposure or color fidelity.
- CAM2 then captured 1920x1080 YUV420M. `v4l2-ctl --get-fmt-video`
  reported `YM12`, three planes, bytes-per-line 1920/480/480, and
  sizeimage 2088960/522240/522240. A five-frame AWISPdemo run logged
  successful dequeue of frames 0 through 4 and clean stream-off in
  about 0.31 seconds. The ISP reports one saved frame set for that run.
- After switching the TDM input to CAM1, CAM1 also captured a
  1920x1080 YUV420M frame with the same reported plane sizes and clean
  stream-off. Full-resolution Y-plane statistics had broad, nonconstant
  values on both ports; padding accounts for values outside the active
  1080 rows.
- A 30-frame CAM1 run at 1920x1080 processed frames 0 through 29 and
  streamed off cleanly in about 1.13 seconds. The output file was
  90,869,760 bytes, corresponding to 29 saved YUV420M frame sets; the
  demo does not save its first dequeued frame in this mode.
- Both ISP runs warned that the IMX219/IMX219_2 tuning profile was
  missing and fell back to an OV13850 profile. They also logged invalid
  actuator-range setup, inability to subscribe to a TDM event, and
  skipped AEWB statistics; full-resolution runs additionally reported
  unsupported sensor flip/temperature and ISP debug-info ioctls. Despite
  these warnings, both channels dequeued frames and streamed off
  successfully. No matching IMX219 ISP profile was found in the
  installed `/mnt`, `/etc`, `/usr/share`, `/usr/lib`, or `/opt` trees.
- A later `v4l2-ctl --all` query on CAM2 `/dev/video8` did not return
  within 30 seconds while its media route/format was not yet configured;
  subsequent SSH, ping, and mDNS attempts failed until the user
  repowered the board. The current boot reports `COLD-BOOT`, but that
  only records the power-on state after the user's power cycle. The
  previous boot's cause cannot be determined: journald is configured
  with `Storage=volatile`, only the current boot ID is retained,
  `/sys/fs/pstore` and `/proc/last_kmsg` are absent, and no crash files
  were found. The kernel advertises a 16-second watchdog, but systemd's
  runtime watchdog is disabled and no prior-boot watchdog/reset status
  is available; there is no retained evidence that it fired or that a
  panic occurred. The `--all` hang and subsequent loss of access are
  temporally related, not
  proof that the query caused a reboot or system hang.
- On the repowered boot, bounded `v4l2-ctl --get-fmt-video` queries
  returned promptly on both video nodes after capture had configured
  them. The earlier broad `--all` query was not repeated. The board
  remained reachable after both full-resolution captures and the
  five-frame CAM2 run.
- The kernel warned that named camera regulator supplies could not be
  obtained. These warnings do not alone prove that the connector power
  rail is absent.
- Read-only pin inspection while VIN was loaded showed PE5 unclaimed,
  PE6 as an input, PE9 as a VIN GPIO input, and PE10 as a GPIO output.
  The external diagnostic report maps CAM2 XCLR to PE9 and MCLK2 to
  PE10, while this DT's `sensor2_pwdn` tuple encodes PE10. Confirm these
  CAM2 pin assignments against the official schematic and BSP before
  relying on a CAM2 probe result. GPIO direction/ownership is not a
  voltage or clock measurement. The successful sensor-ID probe shows
  the control path works at least well enough for detection, despite
  this configuration concern.
- VIN also logged a missing `vipp16` IRQ resource. Whether this blocks
  the selected VINC0 capture path has not been established.
- The available `ai-sdk.tar.gz` was checked: it contains no BSP
  `drivers/vin` source tree, kernel source tree, or OV5647 driver source.
  A matching vendor BSP kernel source tree is still needed for a
  maintainable driver port.

## Other MIPI sensor drivers in this image

The running image has these modules under
`/lib/modules/6.6.98-vendor-sun60iw2/kernel/bsp/drivers/vin/modules/sensor/`.
`modinfo` reports the following I2C aliases:

| Module present | Sensor family | Reported sensor names / aliases | Capture status |
|---|---|---|---|
| `imx219.ko` | Sony IMX219 | `imx219`, `imx219_2` | Both sensors detected; full-resolution ISP-assisted frames captured on both ports; CAM2 delivered five and CAM1 30 consecutive frames; ISP tuning/image quality unverified |
| `gc030a_mipi.ko` | GalaxyCore GC030A | `gc030a_mipi`, `gc030a_mipi_2` | Driver present; CSI capture not verified here |
| `gc05a2_mipi.ko` | GalaxyCore GC05A2 | `gc05a2_mipi`, `gc05a2_mipi_2` | Driver present; CSI capture not verified here |
| `ov13850_mipi.ko` | OmniVision OV13850 | `ov13850_mipi` | Driver present; its sensor descriptor is disabled in this DT |

This is evidence that the image contains sensor-driver code beyond
IMX219 for **GC030A, GC05A2, and OV13850**. It does not prove that a
particular camera module is electrically compatible with CAM1, that
its DT profile is correct, or that frames can be captured. The modules
were inventoried, not loaded or validated with those sensors.

The live DT has an enabled IMX219 descriptor at `sensor@5812000`
(MCLK1, PE6 PWDN, raw address cell `0x20`). An OV13850 descriptor at
`sensor@5812010` is disabled; `sensor@5812020` names `imx219_2`.
Two additional sensor-list nodes are disabled and contain profile
strings `ov5675_mipi_b`, `gc05a2_mipi_b`, `gc5035_mipi_b`,
`ov02a10_mipi_f`, `gc02m1_mipi_f`, and `gc02m2_mipi_f`. These disabled
DT strings are not evidence that matching drivers are installed. Of
those names, only GC05A2 has a corresponding installed module, and the
`gc05a2_mipi_b` profile name does not match the module aliases verbatim.
There is no installed OV5647 or GC2035 module.

Do not treat changing `sensor*_mname` as sufficient to enable another
camera. The sensor profile, I2C address encoding, clock, reset/PWDN
polarity, CSI lane configuration, VINC route, and driver-specific mode
tables must all agree. The CAM1 address cell `0x20` differs from
IMX219's usual 7-bit I2C address `0x10`; confirm the BSP convention
before configuring another sensor.

The live DT has enabled CSI/MIPI blocks and VINC channels 0 and 8; the
earlier claim that this image lacks all VINC nodes was incorrect. The
live media graph registers both IMX219 sensor-to-capture paths, and
full-resolution ISP-assisted capture works on each port when its route
is selected. The two camera routes share one ISP/TDM input and were
tested one at a time; simultaneous dual-camera capture is untested. The
vendor-tested IMX219 is not evidence that the other three driver
families stream successfully here.

## OV5647 control-bus verification

- With PE6 high and PE5 pulsed for reset, `i2cdetect -y 11` shows
  **0x36**; registers `0x300A/0x300B` return **0x56/0x47**.
- Observed module behavior is PE6 low = powered down, high = running.
  Reconcile this with the active-high PWDN flags in the existing IMX219
  DT tuple and the BSP driver's polarity handling before enabling OV5647.
- CAM1's DT address cell is `0x20` for an IMX219 whose usual 7-bit I2C
  address is `0x10`. Confirm the BSP's address convention before
  configuring OV5647; do not assume the field takes `0x36` or `0x6C`.
- The earlier userspace ID test unloaded VIN modules in dependency
  order (`rmmod imx219; rmmod vin_v4l2; rmmod vin_io`), then drove PE6
  and read the chip ID.

## Feasibility and required work

**Verdict: neither inserted IMX219 is shown to be broken. Both answer
the driver's control-bus probe, and both ports deliver full-resolution
frames through the ISP-assisted Armbian capture path; CAM2 delivered
five frames and CAM1 30 frames in succession. Basic frame acquisition
is usable without changing images. Image quality remains unvalidated
because the installed ISP userspace lacks IMX219 tuning and falls back
to OV13850 settings.** Earlier CAM1 failure was observed with the old
sensor and a 15-to-22 cable; after replacing that setup with a native
22-pin sensor, CAM1 probes and captures. That makes the old sensor/cable
path suspect, not proven bad.

Remaining checks, in order:

1. Locate/install the matching IMX219 ISP tuning configuration, then
   check exposure, white balance, color, and image stability in normal
   lighting. The currently installed AWISP configuration falls back to
   OV13850 settings.
2. Test simultaneous dual-camera capture if that use case is required;
   current tests switched the shared TDM/ISP input and ran the ports
   serially.
3. The sensor/cable isolation test is partly complete: the previous
   CAM2 sensor works on CAM1, while the previous CAM1 15-to-22 setup did
   not. To separate the old sensor from the cable/adapter, power off and
   test that old sensor using a known-good compatible 22-to-22 cable, or
   test a known-good 15-to-22 cable with a known-good compatible sensor.
   Do not hot-plug a CSI flex cable.
4. If the same module works on the vendor image but not Armbian, compare
   the kernel build, DTB, overlays, sensor profile, and VIN module
   versions. The current Armbian kernel already identifies as a vendor
   BSP kernel, but may differ in its device tree or packaging.
5. The previous loss of board access cannot currently be attributed to
   the CAM2 metadata query: the old boot journal and kernel crash records
   were not retained. If the problem recurs, capture serial-console
   output or enable persistent journald/pstore before further
   troubleshooting; avoid broad metadata queries while a route is
   unconfigured.

The external report
([allwinner-a733-mipi-csi-diagnostics](https://github.com/alin118/allwinner-a733-mipi-csi-diagnostics))
describes extensive tests with IMX219 and OV5647 and concludes that
the CSI receiver is defective. This is a community investigation, not
vendor documentation, and its hardware-level conclusion has not been
independently verified here. Full-resolution captures on CAM1 and CAM2
show that both CSI/VIN/ISP paths can deliver frames on this Armbian
image. The report's broader conclusion is not fully resolved because
image quality and simultaneous dual-camera operation remain unverified.

For OV5647, the shipped `imx219.ko` cannot be used as its sensor driver:
the sensors have different register maps. A matching BSP OV5647 driver
and correct DT profile would be needed. For any custom sensor-driver
work, the available AI SDK archive does not contain the matching BSP
kernel source.

If CSI bring-up proves blocked, a USB UVC camera remains a practical
fallback; its V4L2 capture can feed the OLED pipeline in
`../lcm_test`. No MIPI capture application exists in this repository.

Switching to a mainline kernel is another possible route, but requires
checking that the selected A733 mainline build supports this board's CSI
receiver and camera routing; upstream OV5647 driver availability alone
does not guarantee that platform pipeline.

## Plain-V4L2 streaming solved (2026-11-08)

`csi_to_lcd` now streams CAM1/CAM2 through plain V4L2 (MMAP) after the
vendor AWISPdemo configures the pipeline. Getting there required:

1. Driver quirks replicated from AWISPdemo, verified by LD_PRELOAD
   ioctl tracing (`/tmp/ioctl_log.so` in the session, reusable):
   - `VIDIOC_S_INPUT(video0, 0)` **before anything else** - binds the
     sensor->vinc route in the driver's bookkeeping. Without it:
     `vinc0 cannot write and read the right addr ... y 0x0` and
     `imx219 is not used, video0 cannot be close!`
   - S_PARM(30fps), private 2-byte ioctl 0xc00256cf, S_FMT YM12.
   - REQBUFS(5), QUERYBUF, mmap, QBUF x5 - all identical to the demo.
   - ISP parameter table: 49 x private ioctl 0x4020565a (32 B each)
     + 1 on h3a.0 + 0xc0105706 + h3a ctrl 0xc00856df + stats subscribe
     0xc00456e1(01), fired at /dev/v4l-subdev14 (sunxi_isp.0) /
     v4l-subdev15 (sunxi_h3a.0) after buffers are queued, before
     STREAMON. Recorded in `isp_replay.c` (generated from the trace;
     static exposure). Without this the ISP drops every frame.
   - ISP event fetch: 0x80885659 (136 B) on isp.0/h3a.0 per iteration;
     frame-done arrives as an ISP event on this BSP.
2. Two plain bugs of ours that had looked like driver wedges:
   - `cam_stream_t.type` was never assigned (memset zero = invalid
     V4L2 buf type) -> REQBUFS EINVAL. G_FMT/S_FMT had worked because
     they set fmt.type explicitly.
   - **DQBUF on multiplane requires `b.length >= num_planes`** (the
     planes array capacity). Ours was 0: frames completed, poll fired
     readable, but `__verify_planes_array` bounced every DQBUF with
     EINVAL and the buffer stayed queued. This is standard vb2
     behaviour (`__verify_planes_array` in videobuf2-v4l2.c), not a
     BSP quirk.
3. Bootstrapping: after a fresh boot the ISP needs one AWISPdemo run
   to initialize; the app now falls back to an "other camera" AWISPdemo
   bootstrap when the selected camera's configure step fails.
   `rmmod/modprobe` is NOT equivalent to a fresh boot: abused driver
   state survives module reloads and can wedge AWISPdemo itself
   (isp0 width/height error reset loops); a full reboot clears it.
4. Media topology (media-ctl -p, driver enables these by default):
   imx219 -> mipi.0 -> csi.0 -> tdm_rx.0 -> isp.0(pad0), isp.0(pad2)
   -> scaler.0 -> vin_cap.0 -> video0; CAM2 mirrors via mipi.1/csi.1/
   scaler.8 -> video8. Only the csi->tdm mux needs enabling, plus
   sensor-side SRGGB10_1X10/1920x1080 pad formats and isp.0:0.
   `setup.sh` (renamed from csi_setup.sh) does the same for manual use.

Result: `./csi_to_lcd` works from a fresh boot with no manual steps
(VIN modules auto-loaded in-app), ~2 fps at SPI 40 MHz (blit-bound).

## CAM2 and dual-camera notes (2026-11-08, continued)

- `-csi 2` wedged the board once: the app proceeded through S_FMT/
  STREAMON after `S_INPUT: No such device` on video8. Root cause: the
  tdm mux had BOTH csi.0->tdm_rx.0 and csi.1->tdm_rx.0 enabled (the
  app's media_setup enabled both); the driver's s_input resolves the
  mux's active source, so video8 could not bind imx219_2. Fix: the
  mux is routed EXCLUSIVELY to the selected camera
  (`media_setup(sel)` + `tdm_switch()`), and S_INPUT failure is now
  fatal (abort before any pipeline setup - never touch the pipeline
  unbound, it can crash the kernel).
- AWISPdemo passes S_INPUT input=0 on BOTH video0 and video8 (traced);
  O_RDWR|O_NONBLOCK on both too.
- Serial console (COM7 115200) shows only ERR-level kernel output on
  this image; the dual-mode deadlock produced NO kernel messages at
  all (silent deadlock), while SSH stayed usable with load 0.00.
- `-csi dual` implementation: the shared ISP/tdm path does not tolerate
  two video nodes in the pipeline at once (second STREAMON stalls
  silently; CAM2's sensor imx219_2 never reached streaming while
  video0 still held buffers). Working design: ONE stream at a time,
  fully closed and re-opened per route switch (STREAMOFF+close ->
  tdm_switch -> open+queue+params+STREAMON), alternating every frame -
  functionally a scripted sequence of AWISPdemo-style runs. ~1 fps per
  camera alternating; simultaneous dual capture would need kernel work
  on the vin/tdm driver.

## Touch I2C error spam on the serial console (2026-11-08)

The serial console (COM7 115200) shows a stream of
`sunxi:twi-2510000.twi [ERR]` lines (dev addr 0x2e = CHSC6417 on TWI0)
while lcm_test/csi_to_lcd run; over SSH the same errors occur but are
only visible via dmesg (the console prints kernel messages live, SSH
does not).

Root cause: the CHSC6417 firmware makes its I2C interface unavailable
for a short window every ~118 ms scan frame (~11% duty). A transfer
that lands in that window is NACKed (state 0x20/0x48); occasionally
the IC aborts mid-byte holding SDA (arbitration lost 0x38, STOP
failed, "twi bus barrier failed, sda is still low") until the
driver's bus-clear recovery succeeds. The BSP twi driver prints 3-6
error lines per failed transfer. Idle = zero errors; the original
app polled at 50 Hz with 2x16-byte reads and 4 retries each, which
maximised both collisions and log spam.

Mitigations applied (touch.c, synced to lcm_test and csi_to_lcd):
- poll interval floor 100 ms, single 4-byte read per sample
  (status+coords only; was 2x16-byte with up to 4 retries each)
- failure backoff 250 ms before the next bus access
- lcm_test TOUCH_POLL_MS 20 -> 100
- TWI0 bus clock lowered 400 kHz -> 100 kHz in the DTB
  (fdtput, backup at /boot/dtb/allwinner/
   sun60i-a733-orangepi-zero3w.dtb.pre-i2c; the pre-SPI3 pristine
   copy remains as .orig) - verified loaded after reboot
Net effect: ~10x fewer transfers; the residual ~1 cascade per ~6 s
beat is intrinsic to the IC firmware (beat between the poll clock and
the 118 ms scan frame) and cannot be polled away at any fixed rate.

Console filtering: `dmesg -n 3` hides kernel ERR from the serial
console while keeping the full log in the dmesg buffer (view over
SSH). Persisted via /etc/systemd/system/twi-console-quiet.service
(enabled; starts after multi-user so boot-time messages stay
verbose). Revert with `systemctl disable --now twi-console-quiet`
and `dmesg -n 7`.

Verified after all changes: touch self-test ACK, all three capture
modes (CAM1 53 frames, CAM2 53 frames, dual 24 frames) and lcm_test
patterns run cleanly; 60 s of serial capture during lcm_test produced
zero console output.

## Touch pacing follow-up fixes (2026-11-08, same day)

The touch pacing changes introduced two regressions, both fixed:

1. lcm_test release delay (~1 s): the release hysteresis needed 6
   consecutive up-samples; at the new 100 ms poll that is 600 ms+.
   Hysteresis is now 2 samples (~200 ms) in both directions, and a
   failed/skipped sample (Touch_Sample() < 0) is treated as
   "no information" instead of "up".
2. csi_to_lcd false touch-downs: touch_poll() used `raw` directly and
   Touch_Sample() returns -1 on a failed/pace-skipped sample - and
   -1 is truthy in C - so every such sample counted towards a
   touch-down, confirmed by stale coordinates from the last successful
   read. Fixed: raw < 0 returns early, and Touch_Sample() confirms
   every down-report with a second identical read (idle/count-0
   answers still need only one read), rejecting the non-atomic
   report updates of the scan cycle.
3. New flag: `csi_to_lcd -no-touch` skips touch init entirely (no
   TWI0 traffic at all). `touch_poll()` is inert without a successful
   self-test.
Verified: 30 s touch-enabled csi_to_lcd run with no finger - 0 false
downs; lcm_test 15 s - 0 false downs; -no-touch runs normally
(55 frames); residual TWI error lines during runs unchanged (~60/20 s,
invisible on console since loglevel 3).

## Touch auto-release design (2026-11-09)

`touch.c` implements an event-driven state machine, not a live
status read: the CHSC6417 firmware writes its report buffer only on
touch events (down / release), and the buffer content persists
indefinitely between events - including across app restarts (verified
with a raw probe: a stale report stayed count=1 with old coordinates
for minutes and was re-read by the next application as a fresh
contact).

Consequences of that design:

- Touch events are detected by report CHANGES, never by the buffer
  content alone. The first read after init is a baseline and is never
  a contact (kills startup phantoms).
- If a release report is missed (the IC's I2C interface is
  unavailable ~50-90 ms every ~930 ms, and app polling backs off on
  failures), the buffer keeps the down-report. A contact whose report
  stops changing for TOUCH_STALE_MS (2000 ms) is therefore treated as
  released (auto-release).

Known limitation: **the long-hold gesture (> 2 s) is not detectable**
- a held finger produces no new reports, so it is indistinguishable
  from a stale report; both auto-release after 2 s. Brief contacts
  whose release report the firmware skips entirely are also cleared
  by the same mechanism. This is acceptable for tap UX today.

Future re-design options (when long-hold or drags are needed):
1. Wire the touch IC INT output to a free GPIO (JP4) and use edge
   interrupts: every report (including release, and hold refreshes if
   the firmware emits them) is then consumed immediately; no polling,
   no missed reports, no stale timeout. Needs one wire + a GPIO irq
   reader (gpiomon) or a kernel touch driver.
2. A kernel input driver for the CHSC6417 consuming the same report
   protocol and exporting /dev/input/eventX; userspace reads evdev
   and the kernel handles retries/recovery.
3. If the firmware exposes a frame-counter/sequence field, use it to
   distinguish stale reports from held contacts and lift the timeout
   (requires identifying such a field in the report area; the current
   4-byte layout has none).

## Incorrect color: root cause and fixability (2026-11-09)

Measured on a captured frame (AWISPdemo 640x480 YUV420M planes):

- V (Cr) mean = 186, U (Cb) mean = 146 (neutral = 128) -> strong
  magenta/red shift.
- Converted with BT.601 full-range (what the app does, matching the
  reported colorspace), center crop: R = 161, G = 32, B = 109 - the
  green channel is nearly wiped out. The image reads as pink/magenta.
- The error is chroma-dominant and far outside any conversion rounding
  error: the app's YUV->RGB conversion is BT.601 and correct.

Root cause (confirmed from the ISP library's own runtime log):

  [ISP_WARN]cannot find imx219_1920_1080_1_0_0 isp config, use
  ov13850_mipi_4224_3136_30_0_0 -> [ov13850_mipi_default_ini_a733]

The ISP602 tuning profiles are compiled into libisp_ini.so /
libisp.so (package libawispapi-isp-602-arm64 1.0.1; vendor branch
"libisp-dev" commit 665dc2e, 2024-12-18). This build contains
profiles ONLY for: gc030a, gc05a2, imx386, ov13850, ov13b10
(imx219 references: 0 in both libraries, verified via strings).
The IMX219 sensor driver exists in the kernel BSP and the vendor
manual lists IMX219 as supported on CAM1/CAM2, but the ISP tuning
for it was omitted from this build. Applying the OV13850 AWB/CCM/
gamma to IMX219 RAW data produces exactly the measured cast.

This is a known ecosystem issue on this SoC: the identical failure
mode (missing per-sensor profile, ov13850 fallback, red/magenta
cast) was reported for the IMX415 camera on Radxa A733 -
github.com/radxa-build/radxa-a733 issue #19 - and was fixed there
by a newer vendor image whose libisp_ini.so carried the missing
profile.

Is it fixable?

YES - but not inside csi_to_lcd. The conversion in the app is
correct; the error is introduced upstream by the ISP applying the
wrong sensor's tuning. Practical paths, in order of practicality:

1. Vendor profile (recommended): ask Orange Pi / Allwinner for an
   ISP602 build of libisp_ini.so that includes an imx219 tuning
   (their ISP build system selects tuning files per sensor in
   libisp/isp_cfg/SENSOR_H/<sensor>; the vendor manual already
   claims IMX219 support, so the tuning file likely exists in
   their SDK). The .so is drop-in replaceable (same package/ABI).
2. Community profile: check newer Radxa A733 images (radxa-build/
   radxa-a733 releases) - their r6 image fixed the analogous
   IMX415 case; if a later build adds imx219, its libisp_ini.so
   can be used on this board.
3. Runtime ini override (medium effort, vendor-sanctioned):
   libisp_ini.so contains a tuning ini parser and looks for
   /mnt/extsd/isp_tuning_param.ini (plus 3a/dynamic/test ini files).
   Authoring an ini with corrected AWB gains / CCM requires the
   ini format (obtainable from the Allwinner SDK's isp_cfg samples)
   and iterative calibration against test scenes.
4. App-level stopgap (NOT recommended as the real fix): a static
   chroma correction in our YUV->RGB step (subtract the measured
   U/V offsets). Scene- and lighting-dependent, fights the running
   AWB; documented here only for completeness.

Blockers for the proper fix today:
- no imx219 ISP602 tuning exists on the image (proven above) and no
  ISP tuning source/tooling is available locally (the AI SDK archive
  checked earlier does not contain the BSP/ISP sources);
- the ISP602 branch is vendor-internal (no public source), so the
  profile binary format would have to be reverse-engineered to hand-
  build a tuning - significant effort and still needs calibration
  data for a real IMX219 module.

## Color FIX deployed (2026-11-09, continued)

The missing IMX219 tuning exists in the public source repo
`radxa/allwinner-isp6xx` (referenced as a submodule of
`radxa-pkg/allwinner-isp6xx`, the Debian packaging for the Allwinner
ISP libraries on A733/T527). It contains the full libisp sources for
ISP520/522/6xx generations, the vendor tuning headers in
`isp6xx/libisp/isp_cfg/SENSOR_H/` - including
`imx219_mipi_1080p_a733.h` (2025/04 Allwinner auto-generation,
iterated to "V26: Balance green/purple, CCM midpoint, brightness+MSC
on" on 2026/06) and `imx219_mipi_3280_2464_a733.h` - plus prebuilt
outputs (`isp6xx/libisp/out/libAWIspApi_602/...`) and the vendor
tuning tool sources (tuning_app).

What was deployed on this board:

1. Radxa's `libAWIspApi_602_1.0.0_arm64.deb` libs
   (libisp.so + libisp_ini.so + libAWIspApi.so) - these include
   imx219 profiles our stock libisp_ini.so lacked. The stock ISP
   lookup name is `imx219_1920_1080_1_0_0` where the third number is
   the FRAME INTERVAL as reported by the pipeline - which is 1
   (unset; G_PARM is unsupported by this BSP driver), so the exact
   30 fps entry never matched and the sensor-name fuzzy fallback
   picked imx219 3280x2464 (correct colors, wrong AE windows).
2. A rebuilt `libisp_ini.so` from the radxa/allwinner-isp6xx source
   with the imx219 1080p entry's fps changed 30 -> 1 so the exact
   match hits and the imx219_mipi_1080p (V26) profile is used.
3. `isp_replay.c` re-recorded from an AWISPdemo run under the new
   library, so the app's static param replay carries the IMX219
   calibration.

Measured result: U mean 120 / V mean 104 (neutral; was U 146 / V 186
with a crushed green channel), exposure normal (Y mean 70).

Rollback: `/usr/lib/aarch64-linux-gnu/libisp{,_ini}.so.orig-ours`
(original Orange Pi 1.0.1 libraries) and
`libisp_ini.so.orig-1.0.1`. The custom libisp_ini.so source patch is
one line in isp6xx/libisp/isp_cfg/isp_ini_parse.c (fps 30 -> 1 on the
imx219 1080p entry).

ISP602 identity: Allwinner's in-house ISP hardware IP (generation
602; A733 datasheet lists the integrated multi-sensor ISP, marketing
calls the AI-assisted generation "AI ISP"; V-series docs call the
hardware "Tiger ISP"). The userspace stack (libisp.so, libisp_ini.so,
libAWIspApi.so) is Allwinner's closed `libisp-dev` branch. Orange Pi's
GitHub (orangepi-xunlong, 47 repos) ships only kernels/build
tooling - the ISP stack travels exclusively as binaries; the tuning
sources surfaced via Radxa's packaging repo instead.

## Green-bias correction + fps recovery (2026-11-09)

After the IMX219 profile fix the chroma became neutral on average
(U 120 / V 104) with a residual green push. Two changes:

1. Static white-balance correction in the app: the measured U/V means
   are neutralized in the YUV->RGB conversion (`wb_u = +8`,
   `wb_v = +24` by default; `-wb u,v` overrides, `-wb 128,128`
   disables). Scene-dependent by nature; the tuning-grade alternative
   is editing the AWB section of the V26 profile in
   radxa/allwinner-isp6xx and rebuilding libisp_ini.so (source and
   build recipe now known).
2. fps regression root cause: the per-frame `isp_events_fetch()`
   (private ioctl 0x80885659 on isp.0/h3a.0) blocks ~830 ms per call
   under the imx219 ISP library - the profiling line read
   "ISP 838ms/frame". It was added during the old DQBUF debugging and
   is NOT required for buffer completion (the real fix was the
   v4l2_buffer.length field). Removed from the loop: fps back to
   5-6 (convert ~60 ms + display ~100 ms per frame at SPI 40 MHz).

Touch sampling also moved to a background pthread (10 Hz): TWI bus
recovery can stall an I2C read for 500+ ms kernel-side, and that stall
previously landed in the render loop ("touch 541ms/frame"). Frame
time is now independent of touch bus health.

## CAM2 tuning fix (2026-11-09, final)

The ISP profile selection runs inside libisp.so's own (stripped,
statically-linked) parser - NOT in libisp_ini.so; replacing
libisp_ini.so alone could never change CAM2's profile. libisp.so's
internal cfg_arr entries are 104-byte inline structs:
{char sensor_name[20]; char isp_cfg_name[50]; int w,h,fps,wdr,ir;
12 reserved bytes}.

Binary patch applied to /usr/lib/aarch64-linux-gnu/libisp.so
(source patch script: patch_full2.py; base file = Radxa
libAWIspApi_602_1.0.0's libisp.so):

1. imx219 1080p entry fps 30 -> 1 (the pipeline reports an unset
   frame interval as 1; CAM1 exact-matches this entry and uses the
   calibrated imx219_1080p profile).
2. The imx219_3280_2464 entry (sensor "imx219") renamed to
   sensor "imx219_2" with geometry 1920x1080@1 and cfg name
   "imx219_1080p_isp_cfg" (CAM2 exact-matches this entry; the
   profile structs are exported symbols the ISP resolves by name -
   verified via the ISP log line:
   "[ISP]find imx219_2_1920_1080_1_0 [imx219_1080p_isp_cfg] isp config").

Result: both cameras use the calibrated IMX219 1080p tuning
("V26: balance green/purple"). Backup of the pre-patch library:
/tmp/libisp_so_radxa (and /usr/lib/aarch64-linux-gnu/libisp.so
was the stock Orange Pi 1.0.1 build before the Radxa deployment).
