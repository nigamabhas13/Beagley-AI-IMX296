#!/usr/bin/env bash
set -euo pipefail

# TIOVX initialization requires privileged access on the tested Arago image.
if [[ $EUID -ne 0 ]]; then
    SCRIPT_PATH="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)/$(basename -- "${BASH_SOURCE[0]}")"
    exec sudo -E bash "$SCRIPT_PATH" "$@"
fi

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
ISP_SCRIPT="$SCRIPT_DIR/stream_isp_h264.sh"

HOST_IP="${1:-}"
PORT="${2:-5000}"

GPIO_CHIP="gpiochip3"
GPIO_OFFSET="8"

LOW_PULSE="${TRIGGER_LOW:-17ms}"
HIGH_PULSE="${TRIGGER_HIGH:-16ms}"

PARAM="/sys/module/imx296/parameters/trigger_mode"
GPIO_PID=""

if [[ -z "$HOST_IP" ]]; then
    echo "Usage: $0 <receiver-ip> [port]"
    exit 2
fi

if [[ ! -f "$ISP_SCRIPT" ]]; then
    echo "ERROR: ISP streaming script not found: $ISP_SCRIPT"
    exit 1
fi

for file in \
    /opt/imaging/imx296/linear/dcc_viss.bin \
    /opt/imaging/imx296/linear/dcc_2a.bin
do
    if [[ ! -r "$file" ]]; then
        echo "ERROR: Required ISP configuration is missing: $file"
        exit 1
    fi
done

if [[ ! -e "$PARAM" ]]; then
    echo "ERROR: IMX296 trigger-mode parameter is missing."
    echo "Install and load the trigger-enabled IMX296 driver first."
    exit 1
fi

cleanup() {
    local status=$?
    trap - EXIT INT TERM

    if [[ -n "$GPIO_PID" ]]; then
        kill -TERM "$GPIO_PID" 2>/dev/null || true
        wait "$GPIO_PID" 2>/dev/null || true
        GPIO_PID=""
    fi

    # Streaming has ended; restore the default for the next stream start.
    printf '0\n' > "$PARAM" 2>/dev/null || true

    exit "$status"
}

trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

# Enable external triggering before the camera starts streaming.
printf '1\n' > "$PARAM"

echo "[INFO] Pipeline: IMX296 -> TI ISP -> Wave5 H.264 -> RTP/UDP"
echo "[INFO] GPIO: $GPIO_CHIP line $GPIO_OFFSET"
echo "[INFO] Low pulse: $LOW_PULSE; high interval: $HIGH_PULSE"
echo "[INFO] Nominal period: 33 ms at default settings"
echo "[INFO] Receiver: $HOST_IP:$PORT"

gpioset --banner -c "$GPIO_CHIP" \
    -t "$LOW_PULSE,$HIGH_PULSE" "$GPIO_OFFSET=0" &
GPIO_PID=$!

sleep 0.3

if ! kill -0 "$GPIO_PID" 2>/dev/null; then
    wait "$GPIO_PID" || true
    GPIO_PID=""
    echo "ERROR: GPIO trigger generator exited unexpectedly."
    exit 1
fi

echo "[INFO] External trigger generator running."
echo "[INFO] Starting ISP stream. Press Ctrl+C to stop."

sh "$ISP_SCRIPT" "$HOST_IP" "$PORT"
