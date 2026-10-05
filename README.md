# BeagleY-AI IMX296 Camera Bring-Up

Reproducible Sony IMX296 camera bring-up and streaming support for the
BeagleY-AI, including CSI2RX, V4L2, TI TIOVX ISP, DCC configuration,
media-controller setup, and GStreamer pipelines.

## Overview

This repository documents a working IMX296 camera configuration on the
BeagleY-AI using the TI Edge AI camera stack.

The project covers:

- Sony IMX296 global-shutter camera bring-up
- V4L2 and media-controller configuration
- TI CSI2RX capture
- TI TIOVX ISP processing
- IMX296 DCC configuration
- Fixed exposure and analogue gain configuration
- GStreamer camera pipelines
- Hardware H.264 encoding
- RTP/UDP streaming

The goal is to provide a reproducible reference for using the IMX296 on
the BeagleY-AI.


## Hardware

### Board

- BeagleY-AI
- TI J722S / AM67A platform
- Arago Linux

### Camera

- Sony IMX296 global-shutter sensor
- Raspberry Pi-compatible IMX296 camera module
- 10-bit Bayer output
- Active resolution: 1456 × 1088
- Pixel format: SBGGR10 / BGGR10
- Target frame rate: 30 FPS


## Working Camera Pipeline

```text
                         +--> TI TIOVX ISP --> NV12 --> VIO / CV
                         |
IMX296 --> CSI-2 --> CSI2RX --> V4L2
                         |
                         +--> imx296-capture --> YUYV 1280x720
                                                    |
                                                    +--> JPEG --> RTP/UDP --> Host
                                                    |
                                                    +--> H.264 --> RTP/UDP --> Host
```

The `imx296-capture` path is the current canonical live-stream path.
The TI TIOVX/DCC path remains available for ISP processing and validation.

## Known Working Configuration

| Parameter | Value |
|---|---|
| Sensor resolution | 1456 × 1088 |
| Bayer format | SBGGR10 / BGGR10 |
| Sensor target rate | 30 FPS |
| Capture node | `/dev/video2` |
| Raw pixel format | `BG10` |
| ISP | TI TIOVX |
| ISP output | NV12 |
| Live-stream geometry | 1280 × 720 |
| Recommended stream | JPEG/RTP over UDP |
| Alternative stream | H.264/RTP over UDP |
| UDP port | 5000 |

## IMX296 ISP / DCC Configuration

The working linear DCC configuration uses:

- VISS black-level correction corresponding to 60 DN
- Identity RGB2RGB matrix
- IMX296 linear VISS configuration
- Separate 2A configuration for the ISP

The generated DCC files are installed on the BeagleY-AI under:

```text
/opt/imaging/imx296/linear/dcc_viss.bin
/opt/imaging/imx296/linear/dcc_2a.bin
```

The DCC binaries are generated from the XML configuration using the TI DCC tools.

## Media Controller Configuration

The IMX296 CSI pipeline is configured through the media controller rather than by forcing the CSI pads directly with `v4l2-ctl`.

The working configuration is:

```bash
sudo media-ctl -d /dev/media1 -V "'imx296 3-001a':0 [fmt:SBGGR10_1X10/1456x1088]"
sudo media-ctl -d /dev/media1 -V "'cdns_csi2rx.30121000.csi-bridge':0 [fmt:SBGGR10_1X10/1456x1088]"
sudo media-ctl -d /dev/media1 -V "'cdns_csi2rx.30121000.csi-bridge':1 [fmt:SBGGR10_1X10/1456x1088]"
sudo media-ctl -d /dev/media1 -V "'30122000.ticsi2rx':0 [fmt:SBGGR10_1X10/1456x1088]"
sudo media-ctl -d /dev/media1 -V "'30122000.ticsi2rx':1 [fmt:SBGGR10_1X10/1456x1088]"
```

After configuration, the IMX296 capture node appears as `/dev/video2` with 10-bit Bayer capture.

## Sensor Exposure and Gain

For stable indoor streaming, the camera is operated with fixed exposure and disabled automatic exposure.

Example working settings:

```bash
sudo v4l2-ctl -d /dev/v4l-subdev2 --set-ctrl=exposure=1500
sudo v4l2-ctl -d /dev/v4l-subdev2 --set-ctrl=analogue_gain=20
```

The IMX296 control ranges observed on this setup are:

```text
exposure:      min=1 max=1048575 step=1
analogue_gain: min=0 max=480 step=1
```

The appropriate exposure and gain depend on the lighting environment. Fixed exposure was found to remove visible brightness fluctuation during indoor streaming.

## UDP Streaming

The recommended live-stream path uses the repository capture utility to convert the IMX296 10-bit Bayer stream into YUYV 1280 × 720, followed by JPEG/RTP over UDP.

### Build the capture tool

On the BeagleY-AI:

```bash
cd tools
gcc -O2 -o imx296-capture imx296-capture.c
```

### JPEG/RTP — Recommended

On the BeagleY-AI:

```bash
./scripts/stream_udp.sh <HOST_IP> [PORT]
```

The default UDP port is `5000`.

The script uses:

- `imx296-capture` at 30 FPS
- BGGR Bayer order
- 1280 × 720 center crop
- JPEG encoding at quality 75
- RTP/JPEG payload type 26
- UDP transport

On the Ubuntu receiver:

```bash
./scripts/receive_udp.sh [PORT]
```

Or run the receiver manually:

```bash
gst-launch-1.0 -v \
    udpsrc port=5000 \
    caps="application/x-rtp,media=video,encoding-name=JPEG,payload=26,clock-rate=90000" ! \
    rtpjpegdepay ! \
    jpegdec ! \
    videoconvert ! \
    autovideosink sync=false
```

### H.264/RTP — Alternative

An H.264/RTP path is also provided using the BeagleY-AI Wave5 hardware encoder.

On the BeagleY-AI:

```bash
./scripts/stream_h264_udp.sh <HOST_IP> [PORT]
```

On the Ubuntu receiver:

```bash
./scripts/receive_h264_udp.sh [PORT]
```

The H.264 path provides lower network bandwidth than JPEG/RTP, but the tested capture-to-encoder pipeline has lower sustained throughput than the JPEG/RTP path.

### Performance Notes

The tested 1280 × 720 JPEG/RTP pipeline achieved approximately 28.6 FPS. JPEG quality 75 produced approximately 119 kB per frame, or roughly 28 Mb/s.

The H.264 encoder itself can sustain 30 FPS at 1280 × 720 when running without competing processing, but the complete capture-to-encoder pipeline measured approximately 25 FPS.

The H.264 streaming geometry is 1280 × 720 because the Wave5 encoder requires dimensions aligned to its supported block size.

For the validated ISP path, TI TIOVX/DCC remains available separately from the canonical live-stream path.

## Reproduction

The bring-up can be reproduced in stages:

1. Apply the appropriate BeagleY-AI device-tree overlay from `dts/`.
2. Build/deploy the IMX296 kernel driver from `kernel/imx296.c`.
3. Configure the CSI media graph using `scripts/setup_media.sh`.
4. Validate raw capture using `scripts/capture_raw.sh`.
5. Generate the DCC binaries using `scripts/generate_dcc.sh` and a compatible TI Imaging installation.
6. Install the generated DCC files under `/opt/imaging/imx296/linear/`.
7. Build `tools/imx296-capture`.
8. Start the JPEG/RTP stream using `scripts/stream_udp.sh`.

### TI Imaging dependency

The DCC generation scripts require the TI Imaging DCC tools, including `dcc_gen_linux`. The TI Imaging package is not redistributed by this repository and must be obtained separately from Texas Instruments.

The repository wrapper expects the TI Imaging installation at `$HOME/ti_imaging/imaging` by default. Set `TI_IMAGING_ROOT` to another installation path when necessary.

## Licensing

This repository contains both project-specific work and Texas Instruments-derived material. See `licenses/README.md` and `licenses/TI-TEXT-FILE-LICENSE.txt` for the licensing information applicable to the TI-derived DCC material.

The IMX296 driver source and device-tree overlays retain their existing source-file license identifiers.

## Helper Scripts

### Configure the CSI pipeline

On the BeagleY-AI:

```bash
./scripts/setup_media.sh
```

### Capture raw frames

On the BeagleY-AI:

```bash
./scripts/capture_raw.sh
```

### Generate DCC binaries

On a host containing the TI Imaging DCC tools:

```bash
./scripts/generate_dcc.sh
```

The generated files are written to `dcc/generated/` and are intentionally ignored by Git.

### Stream from the BeagleY-AI

```bash
./scripts/stream_udp.sh <HOST_IP> [PORT]
```

### Receive on the host PC

```bash
./scripts/receive_udp.sh [PORT]
```
