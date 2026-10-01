#!/bin/bash
set -e

MEDIA_DEV="/dev/media1"

echo "[INFO] Configuring IMX296 CSI media pipeline..."

sudo media-ctl -d "$MEDIA_DEV" -V "'imx296 3-001a':0 [fmt:SBGGR10_1X10/1456x1088]"
sudo media-ctl -d "$MEDIA_DEV" -V "'cdns_csi2rx.30121000.csi-bridge':0 [fmt:SBGGR10_1X10/1456x1088]"
sudo media-ctl -d "$MEDIA_DEV" -V "'cdns_csi2rx.30121000.csi-bridge':1 [fmt:SBGGR10_1X10/1456x1088]"
sudo media-ctl -d "$MEDIA_DEV" -V "'30122000.ticsi2rx':0 [fmt:SBGGR10_1X10/1456x1088]"
sudo media-ctl -d "$MEDIA_DEV" -V "'30122000.ticsi2rx':1 [fmt:SBGGR10_1X10/1456x1088]"

echo "[INFO] Media pipeline configured."
