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
IMX296
   |
   v
CSI-2
   |
   v
TI CSI2RX
   |
   v
V4L2 (/dev/video2)
   |
   v
TI TIOVX ISP
   |
   v
NV12
   |
   +--------------------> VIO / computer vision
   |
   +--> GStreamer --> H.264 --> RTP/UDP --> Host PC
```

## Known Working Configuration

| Parameter | Value |
|---|---|
| Resolution | 1456 × 1088 |
| Bayer format | SBGGR10 / BGGR10 |
| Frame rate | 30 FPS |
| Capture node | `/dev/video2` |
| ISP | TI TIOVX ISP |
| ISP output | NV12 |
| Streaming resolution | 1280 × 720 |
| Encoder | `v4l2h264enc` |
| Transport | RTP / UDP |
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

### BeagleY-AI

Run the following pipeline on the BeagleY-AI. Replace `<HOST_IP>` with the IP address of the receiving computer.

```bash
sudo gst-launch-1.0 -q \
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
    'video/x-raw,format=NV12,width=1456,height=1088' ! \
    videoscale ! \
    'video/x-raw,format=NV12,width=1280,height=720,framerate=30/1' ! \
    queue max-size-buffers=1 max-size-bytes=0 max-size-time=0 leaky=downstream ! \
    v4l2h264enc ! \
    h264parse config-interval=1 ! \
    rtph264pay pt=96 config-interval=1 mtu=1400 ! \
    udpsink host=<HOST_IP> port=5000 sync=false async=false
```

### Ubuntu Receiver

Run the following pipeline on the receiving Ubuntu computer:

```bash
gst-launch-1.0 -v \
    udpsrc port=5000 \
    caps="application/x-rtp,media=video,encoding-name=H264,payload=96,clock-rate=90000" ! \
    rtph264depay ! \
    h264parse ! \
    avdec_h264 ! \
    videoconvert ! \
    autovideosink sync=false
```

## Reproduction

The bring-up can be reproduced in stages:

1. Apply the appropriate BeagleY-AI device-tree overlay from `dts/`.
2. Build/deploy the IMX296 kernel driver from `kernel/imx296.c`.
3. Configure the CSI media graph using `scripts/setup_media.sh`.
4. Validate raw capture using `scripts/capture_raw.sh`.
5. Generate the DCC binaries using `scripts/generate_dcc.sh` and a compatible TI Imaging installation.
6. Install the generated DCC files under `/opt/imaging/imx296/linear/`.
7. Start the RTP/UDP stream using `scripts/stream_udp.sh`.

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
