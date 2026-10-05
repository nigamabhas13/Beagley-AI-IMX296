#!/bin/bash
set -euo pipefail

HOST_IP="${1:?Usage: $0 <HOST_IP> [PORT]}"
PORT="${2:-5000}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CAPTURE="$SCRIPT_DIR/../tools/imx296-capture"

if [ ! -x "$CAPTURE" ]; then
    echo "[ERROR] Capture tool not found or not executable:"
    echo "        $CAPTURE"
    echo
    echo "Build it with:"
    echo "  cd \"$SCRIPT_DIR/../tools\""
    echo "  gcc -O2 -o imx296-capture imx296-capture.c"
    exit 1
fi

echo "[INFO] Streaming IMX296 as JPEG/RTP to ${HOST_IP}:${PORT}"
echo "[INFO] Press Ctrl+C to stop."

sudo "$CAPTURE" \
    --stdout \
    --fps 30 \
    --order bggr \
    --awb \
    --exposure 1350 \
    --gain 30 \
    /dev/media1 | \
    gst-launch-1.0 -q \
        fdsrc fd=0 blocksize=1843200 do-timestamp=true ! \
        rawvideoparse format=yuy2 width=1280 height=720 framerate=30/1 ! \
        gamma gamma=1.7 ! \
        queue max-size-buffers=1 max-size-bytes=0 max-size-time=0 leaky=downstream ! \
        jpegenc quality=75 ! \
        rtpjpegpay pt=26 mtu=1400 ! \
        udpsink host="$HOST_IP" port="$PORT" sync=false async=false
