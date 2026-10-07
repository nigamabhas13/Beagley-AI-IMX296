et -euo pipefail

HOST_IP="${1:-}"
PORT="${2:-5000}"
CAPTURE="$(dirname "$0")/../tools/imx296-capture"

if [[ -z "$HOST_IP" ]]; then
    echo "Usage: $0 <receiver-ip> [port]"
    exit 1
fi

if [[ ! -x "$CAPTURE" ]]; then
    echo "ERROR: capture tool not found: $CAPTURE"
    exit 1
fi

echo "[INFO] IMX296 → YUY2 → gamma → Wave5 H.264 → RTP/UDP"
echo "[INFO] Receiver: ${HOST_IP}:${PORT}"
echo "[INFO] 1280x720 @ 30 FPS"
echo "[INFO] Exposure: 1350"
echo "[INFO] Gain: 30"
echo "[INFO] Gamma: 1.7"
echo "[INFO] H.264 GOP: 30"

"$CAPTURE" \
    --stdout \
    --fps 30 \
    --order bggr \
    --awb \
    --exposure 1350 \
    --gain 30 \
    /dev/media1 | \
gst-launch-1.0 -q \
    fdsrc fd=0 blocksize=1843200 do-timestamp=true ! \
    rawvideoparse \
        format=yuy2 \
        width=1280 \
        height=720 \
        framerate=30/1 ! \
    gamma gamma=1.7 ! \
    'video/x-raw,format=YUY2,width=1280,height=720,framerate=30/1,colorimetry=bt601,chroma-site=mpeg2,pixel-aspect-ratio=1/1,interlace-mode=progressive' ! \
    v4l2h264enc \
        extra-controls="controls,video_gop_size=30" ! \
    h264parse ! \
    rtph264pay \
        pt=96 \
        config-interval=1 \
        aggregate-mode=zero-latency ! \
    udpsink \
        host="$HOST_IP" \
        port="$PORT" \
        sync=false \
        async=false
