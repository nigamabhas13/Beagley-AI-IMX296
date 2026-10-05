#!/bin/bash
set -euo pipefail

PORT="${1:-5000}"

echo "[INFO] Listening for IMX296 RTP/H.264 stream on UDP port ${PORT}"
echo "[INFO] Press Ctrl+C to stop."

gst-launch-1.0 -v udpsrc port="$PORT" caps="application/x-rtp,media=video,encoding-name=H264,payload=96,clock-rate=90000" ! rtph264depay ! h264parse ! avdec_h264 ! videoconvert ! autovideosink sync=false
