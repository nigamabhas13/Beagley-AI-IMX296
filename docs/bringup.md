# IMX296 Bring-Up on BeagleY-AI

This document describes the validated Sony IMX296 global-shutter camera bring-up used with the BeagleY-AI / J722S (AM67A) platform and the Arago Linux environment used during development.

The repository contains the project-specific IMX296 kernel driver source, device-tree overlay sources, DCC source/configuration, and helper scripts. Vendor kernel/device-tree trees and the Texas Instruments Imaging package are not redistributed here.

## 1. Validated setup

| Component | Validated configuration |
| --- | --- |
| Board | BeagleY-AI |
| SoC | J722S / AM67A |
| OS | Arago Linux 2025.01 |
| Running kernel | `6.12.33-g336b1a0a004d` |
| Architecture | `aarch64` |
| Camera | Sony IMX296 global shutter |
| Sensor mode | 1456 x 1088, 10-bit Bayer, 30 FPS |
| Sensor bus format | `SBGGR10_1X10` |
| CSI capture node | `/dev/video2` |
| Raw V4L2 pixel format | `BG10` |
| ISP output | NV12 |
| Hardware H.264 encoder | Wave5 / `v4l2h264enc` |
| Network stream | JPEG/RTP over UDP, port 5000 (current canonical stream); H.264/RTP remains an alternative |
| Host-side GStreamer | 1.22.x on the receiver used during validation |

The board used during validation had the IMX296 connected to the camera module connector(s) described by the CSI0 and CSI1 overlays in `dts/`.

The current live-stream implementation uses the repository's `imx296-capture` utility. The TI TIOVX/DCC path remains a validated ISP path, but its older end-to-end H.264 pipeline is not the current performance baseline.

## 2. Repository layout

```text
.
├── README.md
├── dts/
│   ├── k3-am67a-beagley-ai-csi0-imx296.dtso
│   └── k3-am67a-beagley-ai-csi1-imx296.dtso
├── kernel/
│   └── imx296.c
├── dcc/
│   ├── generated/
│   └── linear/
├── licenses/
├── scripts/
└── docs/
    └── bringup.md
```

Generated `.dtbo`, `.dtb`, `.ko`, raw captures, and other build/runtime artifacts are intentionally excluded from the public repository where appropriate.

## 3. Prerequisites

The validated build environment used:

- a Linux host/board with the kernel source tree corresponding to the target kernel
- `make`
- `dtc` / `fdtoverlay` from DTC 1.7.x
- the TI Imaging package for DCC generation and TIOVX ISP use
- the target kernel configured with:

```text
CONFIG_MEDIA_SUPPORT=m
CONFIG_VIDEO_V4L2_I2C=y
CONFIG_VIDEO_V4L2_SUBDEV_API=y
CONFIG_VIDEO_IMX296=m
```

The IMX296 driver is therefore built as an out-of-tree kernel module in the validated setup.

## 4. Build the IMX296 kernel module

The validated kernel build tree on the board is:

```text
~/kernel/kbuild
```

The driver source in this repository is copied to an external-module directory containing:

```make
obj-m += imx296.o
```

For a generic checkout, build the external module with the matching kernel tree:

```bash
cd /path/to/imx296mod
make -C /path/to/kernel/tree M=$PWD KBUILD_MODPOST_WARN=1 modules
```

On the validated BeagleY-AI filesystem, the same build was reproduced from:

```bash
cd ~/work/imx296/imx296mod
make -C ~/kernel/kbuild M=$PWD KBUILD_MODPOST_WARN=1 modules
```

A build produces:

```text
imx296.o
imx296.mod.o
imx296.ko
modules.order
Module.symvers
```

### Why `KBUILD_MODPOST_WARN=1` is present

The validated `~/kernel/kbuild` tree did not contain a kernel-level `Module.symvers`. Without it, `modpost` stopped with unresolved-symbol errors. With `KBUILD_MODPOST_WARN=1`, those unresolved symbols are emitted as warnings and the module is linked successfully.

A fully built kernel tree that provides a matching `Module.symvers` is preferable for a conventional external-module build; this repository documents the exact procedure that was validated on the target board.

### Verify the module matches the running kernel

The validated module reported:

```text
vermagic=6.12.33-g336b1a0a004d SMP preempt mod_unload aarch64
```

Check yours with:

```bash
strings /path/to/imx296.ko | grep -m1 vermagic
uname -r
```

The kernel version in `vermagic` should correspond to the running target kernel.

## 5. Device-tree overlays

The repository contains two `/plugin/` overlays:

```text
dts/k3-am67a-beagley-ai-csi0-imx296.dtso
dts/k3-am67a-beagley-ai-csi1-imx296.dtso
```

### CSI0 overlay

CSI0 uses `main_i2c2` and the module-power GPIO associated with the CSI0 camera connector. The overlay provides the IMX296's mandatory `avdd`, `dvdd`, and `ovdd` supplies, a fixed 54 MHz input clock, and the CSI2RX/DPHY graph configuration.

The module carries its own 54 MHz oscillator; the connector does not provide an external MCLK. The overlay therefore supplies a fixed-clock node to the driver.

The `avdd` supply in the overlay represents the camera-module power-enable path. The module generates its own sensor-domain supplies, so the `dvdd` and `ovdd` regulators are represented as always-on/dummy supplies to satisfy the IMX296 driver's bulk-supply requirements.

The CSI0 overlay intentionally does not provide a reset GPIO for the module-power pin; using that pin as a conventional active-high sensor reset would conflict with the module power-enable behavior.

### CSI1 overlay

CSI1 uses `main_i2c0`. The validated overlay contains two board-specific fixes:

1. A longer module-power startup delay (`2 s`) was required on the validated hardware. A shorter delay caused the sensor probe to fail with `-110`, while the 2 s delay allowed:

```text
imx296 3-001a: found IMX296LQ (...C)
```

2. The CSI1 interrupt routing was corrected to:

```dts
interrupts = <0 146 4>, <0 147 4>;
```

The overlay comments document the measured/validated reason for this change.

The CSI1 overlay also explicitly sets the MIPI switch state so the lanes are routed to CSI1:

```dts
oe-pin {
    default-state = "off";
};
sel-pin {
    default-state = "on";
};
```

## 6. Compile the overlays

From the repository root:

```bash
dtc -@ -I dts -O dtb \
  -o /tmp/k3-am67a-beagley-ai-csi0-imx296.dtbo \
  dts/k3-am67a-beagley-ai-csi0-imx296.dtso
```

```bash
dtc -@ -I dts -O dtb \
  -o /tmp/k3-am67a-beagley-ai-csi1-imx296.dtbo \
  dts/k3-am67a-beagley-ai-csi1-imx296.dtso
```

The validated `dtc` was `DTC v1.7.0+` and each generated overlay was approximately 3.2 KiB.

### Expected DTC warnings

The validated sources currently produce non-fatal warnings.

CSI0:

```text
Warning (graph_child_address): ... ports: graph node has single child node 'port@0' ...
```

CSI1:

```text
Warning (interrupts_property): ... Missing interrupt-parent
Warning (graph_child_address): ... ports: graph node has single child node 'port@0' ...
```

`dtc` still exits successfully and produces the `.dtbo` files. These warnings are intentionally not hidden in this documentation.

## 7. Merge the overlays into a DTB

The validated flow was to start from a base BeagleY-AI DTB and apply the IMX296 overlays offline using `fdtoverlay`.

The vendor/base DTB is not redistributed in this repository. Use the base DTB corresponding to the target software image.

For CSI0:

```bash
fdtoverlay \
  -i base.dtb \
  -o merged.dtb \
  k3-am67a-beagley-ai-csi0-imx296.dtbo
```

For both CSI0 and CSI1:

```bash
fdtoverlay \
  -i merged.dtb \
  -o merged-both.dtb \
  k3-am67a-beagley-ai-csi1-imx296.dtbo
```

These two merge operations were verified byte-for-byte on the validated board. The generated files matched the saved reference files used during bring-up.

## 8. Install/select the validated DTB

On the validated BeagleY-AI, the resulting both-camera DTB was installed as:

```text
/boot/dtb/k3-am67a-beagleyai.dtb
```

A copy of the original board DTB and intermediate camera configurations was retained during development under names such as:

```text
k3-am67a-beagleyai.dtb.orig
k3-am67a-beagleyai.dtb.csi0only
k3-am67a-beagleyai.dtb.both1
k3-am67a-beagleyai.dtb.both3
```

The exact firmware-side FDT handoff mechanism was not recovered from the shell history and is intentionally not described here as a specific U-Boot or GRUB procedure. The validated board did boot the IMX296-enabled device tree, and the live device tree contained the expected custom nodes.

After boot, verify the live model:

```bash
cat /proc/device-tree/model; echo
```

Expected:

```text
BeagleBoard.org BeagleY-AI
```

You can also inspect the live device-tree filesystem:

```bash
ls /sys/firmware/devicetree/base | grep -E 'imx296|cam[01]|mipi-switch'
```

The validated live tree contained the custom IMX296 clocks, camera regulators, and MIPI-switch nodes.

## 9. Load and verify the IMX296 driver

Load the newly built `imx296.ko` using the module-loading method appropriate to the target image, then inspect the kernel log:

```bash
dmesg | grep -i imx296
```

A successful probe on the validated board included a line similar to:

```text
imx296 3-001a: found IMX296LQ (36.xC)
```

The driver is an out-of-tree module, so the kernel may report that loading it taints the kernel. That is expected for this development setup.

## 10. Configure the media graph

The validated raw path uses the IMX296 in 1456 x 1088 10-bit Bayer mode.

Run:

```bash
sudo media-ctl -d /dev/media1 -V "'imx296 3-001a':0 [fmt:SBGGR10_1X10/1456x1088]"
sudo media-ctl -d /dev/media1 -V "'cdns_csi2rx.30121000.csi-bridge':0 [fmt:SBGGR10_1X10/1456x1088]"
sudo media-ctl -d /dev/media1 -V "'cdns_csi2rx.30121000.csi-bridge':1 [fmt:SBGGR10_1X10/1456x1088]"
sudo media-ctl -d /dev/media1 -V "'30122000.ticsi2rx':0 [fmt:SBGGR10_1X10/1456x1088]"
sudo media-ctl -d /dev/media1 -V "'30122000.ticsi2rx':1 [fmt:SBGGR10_1X10/1456x1088]"
```

The working capture node was:

```text
/dev/video2
```

with:

```text
BG10
1456 x 1088
bytes/line: 2912
```

The media graph can be repaired to this known-good state using `scripts/setup_media.sh`.

## 11. Validate direct raw capture

The raw capture helper is:

```bash
scripts/capture_raw.sh /tmp/imx296-test.raw
```

The underlying validated command is:

```bash
sudo v4l2-ctl -d /dev/video2 \
  --set-fmt-video=width=1456,height=1088,pixelformat=BG10 \
  --stream-mmap=4 \
  --stream-count=5 \
  --stream-to=/tmp/imx296-test.raw
```

A successful capture produces multiple raw IMX296 frames without a V4L2 stream failure.

## 12. TIOVX ISP / DCC path

The validated ISP pipeline uses TI TIOVX with the custom IMX296 sensor registration:

```text
SENSOR_SONY_IMX296_RPI
```

The deployed TIOVX sensor module contains a DCC ID mapping for that sensor name. The runtime library was verified to contain the string:

```text
SENSOR_SONY_IMX296_RPI
```

The working DCC files were installed under:

```text
/opt/imaging/imx296/linear/dcc_viss.bin
/opt/imaging/imx296/linear/dcc_2a.bin
```

The validated TIOVX pipeline was:

```bash
sudo gst-launch-1.0 -e -q \
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
    fakesink sync=false
```

### DCC generation

The repository contains the TI-derived DCC XML/LUT source used for the validated linear configuration under `dcc/linear/`.

Use:

```bash
scripts/generate_dcc.sh
```

The wrapper expects the TI Imaging source tree at either:

```text
$HOME/ti_imaging/imaging
```

or the path supplied through `TI_IMAGING_ROOT`.

The wrapper intentionally does not treat `dcc_gen_linux` exit status 68 as an immediate build failure because the tool returned that status after successfully generating the expected `[OK!]` output during validation.

Generated `.bin` files are ignored by Git. The validated DCC outputs were:

```text
dcc_viss.bin  19 KiB
md5: da72a896deb1cd0693586f56cbeb8ef2

dcc_2a.bin    5.7 KiB
md5: d4ac1d9ac9fe87d2b82f587f6ba28d35

dcc_ldc.bin   2.3 KiB
md5: 3a07ac6784eabed3f8ba84d13392c179
```

## 13. Known-good image/ISP settings

The validated linear DCC configuration uses 60 DN VISS black-level correction (`BLC60`). The corresponding DCC source uses:

```text
-60, -60, -60, -60
```

for the VISS BLC path.

The tested camera controls included:

```text
vertical_blanking: 1162
exposure:           1350 to 1500 during validation
analogue_gain:      0 to 20 during validation
test_pattern:       0
pixel_rate:         118.8 MHz
```

An exposure around `1350` removed the visible flicker encountered during earlier testing. A later validated stream used approximately `1500` exposure with analogue gain `20`.

The current repository configuration intentionally does not pursue additional color-correction tuning. For visual-inertial use, the luminance/structure of the image is more important than matching RGB color appearance.

## 14. Current live-stream path

The current validated low-complexity streaming path uses the project-specific `imx296-capture` utility rather than the older TIOVX-to-H.264 pipeline as the canonical live-stream command.

The canonical capture utility is now:

```text
~/work/imx296/tools/imx296-capture
```

It performs the software demosaic/conversion path and now crops the 1456-wide sensor image to 1280x720 internally. The crop is not a software rescale, so this avoids a separate `videoscale` stage.

The canonical utility also clamps the post-white-balance Y/U/V values to the valid 8-bit range before writing them. This fixes the earlier `uint8_t` wraparound defect where gain values above 255 could wrap and create false pink/green pixels. The older `imx296-capture-clamped` and `imx296-capture-green-test` programs remain experimental/reference variants and are not the canonical tool.

### JPEG/RTP stream

The best measured live-stream path is JPEG over RTP/UDP. During validation it reached approximately 28.6 FPS over a 17.3 s run at 1280x720 with AWB locked (`R=G=B`). It is therefore close to 30 FPS, but the measured result should not be described as a sustained 30 FPS stream.

On the BeagleY-AI:

```bash
~/work/imx296/tools/imx296-capture --stdout --fps 30 --order bggr --awb /dev/media1 | \
gst-launch-1.0 -q fdsrc fd=0 blocksize=1843200 do-timestamp=true ! \
  rawvideoparse format=yuy2 width=1280 height=720 framerate=30/1 ! \
  queue max-size-buffers=1 max-size-bytes=0 max-size-time=0 leaky=downstream ! \
  jpegenc quality=75 ! \
  rtpjpegpay pt=26 mtu=1400 ! \
  udpsink host=HOST_IP port=5000 sync=false async=false
```

Replace `HOST_IP` with the receiving machine's address. A quality around 60 can be used when lower bandwidth is preferred. At quality 75, the measured stream was approximately 119 kB/frame, or about 28 Mb/s.

On the receiving Linux host:

```bash
gst-launch-1.0 -v \
  udpsrc port=5000 \
  caps="application/x-rtp,media=video,encoding-name=JPEG,payload=26,clock-rate=90000" ! \
  rtpjpegdepay ! \
  jpegdec ! \
  videoconvert ! \
  autovideosink sync=false
```

`rtpjpegpay` does not have a `config-interval` property; adding one results in a pipeline error.

### H.264/RTP alternative

H.264 is useful when bandwidth is more important than frame rate, but the combined software capture/demosaic path measured approximately 25 FPS at 1280x720 during validation. The Wave5 hardware encoder itself was measured at 30 FPS when used without the competing software path.

A representative H.264 path is:

```text
imx296-capture
    -> rawvideoparse (YUY2 1280x720)
    -> queue
    -> videoconvert
    -> NV12 1280x720
    -> v4l2h264enc
    -> h264parse
    -> rtph264pay
    -> udpsink
```

The earlier TIOVX -> NV12 -> `videoscale` -> Wave5 pipeline remains useful for validating the TI ISP/TIOVX path, but it should not be treated as the current performance baseline for live streaming.

### Performance notes

The measured behavior on the validated board was:

| Path | Measured result |
| --- | ---: |
| Capture tool alone (software demosaic) | 446 frames / 15.3 s = 29.2 FPS |
| Wave5 encoder, live 720p30, no competing software path | 450 / 450 = 30 FPS |
| Capture tool -> parse -> videoconvert -> H.264 encoder | 248 / 15 s = 16.5 FPS |
| Same H.264 path with a non-leaky queue | ~25 FPS, with frame loss |
| Capture tool -> parse -> JPEG -> RTP/UDP | 495 / 17.3 s = 28.6 FPS |

The stream queues in the original H.264 command were leaky, so frames could be discarded when the pipeline could not keep up. This is a source of apparent lag even when the encoder itself is capable of the target rate.

The hardware H.264 encoder also expects dimensions compatible with its alignment requirements; the validated streaming paths use 1280x720 after the capture tool's crop.

## 15. Host-side receiver

For the current JPEG/RTP stream, use:

```bash
gst-launch-1.0 -v \
  udpsrc port=5000 \
  caps="application/x-rtp,media=video,encoding-name=JPEG,payload=26,clock-rate=90000" ! \
  rtpjpegdepay ! \
  jpegdec ! \
  videoconvert ! \
  autovideosink sync=false
```

For the older H.264/TIOVX path documented above, the receiver remains:

```bash
scripts/receive_udp.sh 5000
```

with:

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

## 16. Troubleshooting

### Camera node exists but the media graph is wrong

Reapply the validated media-bus formats with:

```bash
scripts/setup_media.sh
```

The expected raw format is:

```text
SBGGR10_1X10 / 1456x1088
```

### External module build stops in `modpost`

Check whether the kernel tree contains `Module.symvers`:

```bash
ls -lh /path/to/kernel/tree/Module.symvers
```

On the validated tree it was missing, so the working command included:

```bash
KBUILD_MODPOST_WARN=1
```

### IMX296 probe fails on CSI1 with `-110`

The validated CSI1 overlay uses a 2-second camera-module power startup delay. Check that the CSI1 overlay is the one being applied and that its power regulator node contains the validated delay.

### CSI1 reports an IRQ request/flags conflict

Check that the CSI1 overlay uses:

```dts
interrupts = <0 146 4>, <0 147 4>;
```

rather than the conflicting interrupt routing found in the original vendor configuration.

### TIOVX fails with a `/dev/mem` or permission error

The validated ISP pipeline was launched with `sudo` because the target's `/dev/mem` permissions require root access for the TIOVX path.

### DCC files are missing

Regenerate them on a system with the TI Imaging package:

```bash
scripts/generate_dcc.sh
```

The TI Imaging package itself is not redistributed by this repository.

## 17. Validation checklist

A fresh bring-up should reach these checkpoints in order:

```text
[ ] CONFIG_VIDEO_IMX296=m
[ ] imx296.ko builds against the target kernel tree
[ ] CSI0 and/or CSI1 .dtso compiles with dtc -@
[ ] overlay merge completes with fdtoverlay
[ ] IMX296 probes at I2C address 0x1a
[ ] media graph is SBGGR10_1X10 / 1456x1088
[ ] /dev/video2 exposes BG10 / 1456x1088
[ ] raw V4L2 capture succeeds
[ ] TIOVX ISP opens with SENSOR_SONY_IMX296_RPI (when using the ISP path)
[ ] DCC files load successfully (for the ISP path)
[ ] ISP produces NV12 (for the ISP path)
[ ] `imx296-capture` produces 1280x720 YUY2 frames
[ ] JPEG/RTP stream reaches the host
[ ] Optional Wave5 H.264 path starts
```

## 18. Reproducibility notes

The following parts of the bring-up were directly reproduced from the validated board state during documentation work:

- external IMX296 module compilation using the matching kernel tree
- CSI0 `.dtso` compilation
- CSI1 `.dtso` compilation
- CSI0 overlay merge
- CSI0 + CSI1 overlay merge

The two `fdtoverlay` operations reproduced the saved reference DTBs byte-for-byte.

The exact historical shell commands used to install the final DTB and to perform the first successful module load were not recoverable from the available shell history. The documentation therefore records the validated resulting paths and runtime state without inventing an unsupported bootloader command sequence.

## 19. Licensing / vendor dependencies

This repository mixes project-specific source with TI-derived DCC material. See `licenses/README.md` and `licenses/TI-TEXT-FILE-LICENSE.txt` for the applicable TI text-file license notice.

The TI Imaging package, its runtime components, and the base vendor kernel/device-tree package are external dependencies and are not redistributed here.
