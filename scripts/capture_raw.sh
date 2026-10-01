#!/bin/bash
set -euo pipefail

OUT="${1:-/tmp/imx296-test.raw}"

echo "[INFO] Capturing IMX296 raw frames to: $OUT"

sudo v4l2-ctl -d /dev/video2 --set-fmt-video=width=1456,height=1088,pixelformat=BG10 --stream-mmap=4 --stream-count=5 --stream-to="$OUT"

ls -lh "$OUT"
echo "[INFO] Raw capture successful."
