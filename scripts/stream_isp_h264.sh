#!/bin/sh
set -eu

HOST_IP="${1:-}"
PORT="${2:-5000}"

if [ -z "$HOST_IP" ]; then
    echo "Usage: sudo $0 <laptop-ip> [port]"
    exit 1
fi

echo "[INFO] Configuring IMX296 and CSI media formats..."

media-ctl -d /dev/media1 \
    -V '"imx296 3-001a":0 [fmt:SBGGR10_1X10/1456x1088 field:none]' &&
media-ctl -d /dev/media1 \
    -V '"cdns_csi2rx.30121000.csi-bridge":0 [fmt:SBGGR10_1X10/1456x1088 field:none]' &&
media-ctl -d /dev/media1 \
    -V '"cdns_csi2rx.30121000.csi-bridge":1 [fmt:SBGGR10_1X10/1456x1088 field:none]' &&
media-ctl -d /dev/media1 \
    -V '"30122000.ticsi2rx":0 [fmt:SBGGR10_1X10/1456x1088 field:none]' &&
media-ctl -d /dev/media1 \
    -V '"30122000.ticsi2rx":1 [fmt:SBGGR10_1X10/1456x1088 field:none]'

echo "[INFO] Starting IMX296 ISP H.264 stream"
echo "[INFO] Receiver: ${HOST_IP}:${PORT}"
echo "[INFO] Output: 1280x720 @ 30 FPS"

exec gst-launch-1.0 -e \
    v4l2src device=/dev/video2 io-mode=5 ! \
    'video/x-bayer,format=bggr10,width=1456,height=1088,framerate=30/1' ! \
    tiovxisp \
        sensor-name=SENSOR_SONY_IMX296_RPI \
        dcc-isp-file=/opt/imaging/imx296/linear/dcc_viss.bin \
        format-msb=9 \
        sink_0::dcc-2a-file=/opt/imaging/imx296/linear/dcc_2a.bin \
        sink_0::ae-mode=2 \
        sink_0::awb-mode=0 \
        sink_0::device=/dev/v4l-subdev2 ! \
    videocrop left=88 right=88 top=184 bottom=184 ! \
    videorate drop-only=true ! \
    'video/x-raw,format=NV12,width=1280,height=720,framerate=30/1' ! \
    v4l2h264enc \
        extra-controls="controls,video_gop_size=30" ! \
    h264parse ! \
    rtph264pay pt=96 config-interval=1 aggregate-mode=zero-latency ! \
    udpsink host="$HOST_IP" port="$PORT" sync=false async=false
