#!/bin/bash
set -euo pipefail

HOST_IP="${1:?Usage: $0 <HOST_IP> [PORT]}"
PORT="${2:-5000}"

echo "[INFO] Streaming IMX296 to ${HOST_IP}:${PORT}"
echo "[INFO] Press Ctrl+C to stop."

sudo gst-launch-1.0 -q v4l2src device=/dev/video2 io-mode=5 ! 'video/x-bayer,format=bggr10,width=1456,height=1088,framerate=30/1' ! tiovxisp sensor-name=SENSOR_SONY_IMX296_RPI dcc-isp-file=/opt/imaging/imx296/linear/dcc_viss.bin format-msb=9 sink_0::dcc-2a-file=/opt/imaging/imx296/linear/dcc_2a.bin sink_0::ae-mode=2 sink_0::awb-mode=0 sink_0::device=/dev/v4l-subdev2 ! 'video/x-raw,format=NV12,width=1456,height=1088' ! videoscale ! 'video/x-raw,format=NV12,width=1280,height=720,framerate=30/1' ! queue max-size-buffers=1 max-size-bytes=0 max-size-time=0 leaky=downstream ! v4l2h264enc ! h264parse config-interval=1 ! rtph264pay pt=96 config-interval=1 mtu=1400 ! udpsink host="$HOST_IP" port="$PORT" sync=false async=false
