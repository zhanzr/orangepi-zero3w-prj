#!/bin/bash
# CAM1/CAM2 media pipeline setup for csi_to_lcd
# (run once per boot, as root, AFTER the VIN modules load: modprobe vin_io imx219 vin_v4l2)
set -e
modprobe vin_io 2>/dev/null || true
modprobe imx219 2>/dev/null || true
modprobe vin_v4l2 2>/dev/null || true
sleep 1
M="media-ctl -d /dev/media0"
# CAM1 route: imx219 -> mipi.0 -> csi.0 -> tdm_rx.0 -> isp.0 -> scaler.0 -> vin_cap.0 -> /dev/video0
$M -l '"imx219":0->"sunxi_mipi.0":0[1]' 2>/dev/null || true
$M -l '"sunxi_mipi.0":1->"sunxi_csi.0":0[1]' 2>/dev/null || true
# CAM2 route: imx219_2 -> mipi.1 -> csi.1 -> tdm_rx.0 (shared ISP input) -> vin_cap.8 -> /dev/video8
$M -l '"imx219_2":0->"sunxi_mipi.1":0[1]' 2>/dev/null || true
$M -l '"sunxi_mipi.1":1->"sunxi_csi.1":0[1]' 2>/dev/null || true
$M -l '"sunxi_csi.1":1->"sunxi_tdm_rx.0":0[1]' 2>/dev/null || true
# CAM1 formats through the chain
$M --set-v4l2 '"imx219":0[SRGGB10_1X10/1920x1080]'
$M --set-v4l2 '"sunxi_mipi.0":0[SRGGB10_1X10/1920x1080]'
$M --set-v4l2 '"sunxi_csi.0":0[SRGGB10_1X10/1920x1080]'
$M --set-v4l2 '"sunxi_tdm_rx.0":0[SRGGB10_1X10/1920x1080]'
$M --set-v4l2 '"sunxi_isp.0":0[SRGGB10_1X10/1920x1080]'
echo "media pipeline configured (CAM1+CAM2 routes; TDM input selects per capture)"
