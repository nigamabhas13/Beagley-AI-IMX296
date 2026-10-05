#!/bin/bash
set -euo pipefail

PORT="${1:-5000}"

echo "[INFO] Listening for IMX296 RTP/JPEG stream on UDP port ${PORT}"
echo "[INFO] Press Ctrl+C to stop."

gst-launch-1.0 -v \
    udpsrc port="$PORT" \
    caps="application/x-rtp,media=video,encoding-name=JPEG,payload=26,clock-rate=90000" ! \
    rtpjpegdepay ! \
    jpegdec ! \
    videoconvert ! \
    autovideosink sync=false
