# csi_to_lcd - IMX219 camera to CO5300 OLED

Streams one IMX219 camera through the Armbian vendor ISP/VIN pipeline to
the CO5300 368x448 panel. The default command, `./csi_to_lcd`, tries
CAM1 first and falls back to CAM2. It captures 640x480 YUV420M, converts
to RGB565, center-crops/scales to the 368x424 image area, and uses the
bottom 24 rows for status. The app captures through plain V4L2 after a
one-frame AWISPdemo initialization.

## Requirements

Run as root, or with access to the VIN, camera, SPI3, and I2C0 devices.
The app loads the VIN and IMX219 modules, configures the media links and
formats, and initializes the selected camera through `/usr/bin/AWISPdemo`.
`setup.sh` is optional; it can configure the media pipeline manually.

## Usage

```
csi_to_lcd [-csi auto|1|2|dual] [-s speed_kHz] [-b brightness_hex]
           [-W capture_width] [-H capture_height] [-n frames]
```

- `-csi auto` selects CAM1 if detected, otherwise CAM2 (default).
- `-csi 1` or `-csi 2` selects a specific connector.
- `-csi dual` alternates the two cameras; the shared ISP path is switched
  between frames, not operated simultaneously.
- `-W` and `-H` set the capture size (defaults: 640x480).
- `-s` sets the requested SPI clock (default: 40000 kHz).
- `-b` sets panel brightness (default: `0x28`).
- `-n` stops after the requested number of displayed frames; without it,
  the app runs until interrupted.

Touch events from the CHSC6417 on I2C0 are printed to the console.

## Image format and known limitations

The capture node reports `YM12` (YUV420M), `colorspace=JPEG`, and
default quantization. For that metadata, the app uses full-range BT.601
YUV-to-RGB conversion. It also honors the negotiated plane strides and
uses fixed-point bilinear scaling instead of nearest-neighbor sampling.
OLED rows are sent in batched SPI messages while retaining a separate
chip-select transaction for each row. ISP events are drained
non-blockingly so waiting for an idle event does not stall frame display.
The status log includes approximate capture, conversion, display, ISP,
and touch time per frame.

The installed ISP userspace does not include an IMX219 tuning profile
and falls back to OV13850 settings. Capture and frame delivery work,
but exposure, white balance, color fidelity, and long-term image quality
are not calibrated. Low light or a poor lens focus can still make the
displayed image hard to recognize. The panel update rate is limited by
the SPI row transfers and is lower than the sensor's capture rate. On
the tested board, these changes raised display throughput from about
2 fps to roughly 5 fps at 40 MHz; actual results vary with scene and
touch-bus latency.
