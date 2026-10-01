#!/bin/bash
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DCC_XML_DIR="$REPO_ROOT/dcc/linear"
TI_IMAGING_ROOT="${TI_IMAGING_ROOT:-$HOME/ti_imaging/imaging}"
DCC_TOOL_PATH="$TI_IMAGING_ROOT/tools/dcc_tools"
OUT_DIR="$REPO_ROOT/dcc/generated"

if [[ ! -x "$DCC_TOOL_PATH/dcc_gen_linux" ]]; then
    echo "[ERROR] dcc_gen_linux not found:"
    echo "        $DCC_TOOL_PATH/dcc_gen_linux"
    echo
    echo "Set TI_IMAGING_ROOT to the TI imaging installation containing tools/dcc_tools."
    exit 1
fi

mkdir -p "$OUT_DIR"
rm -f "$OUT_DIR"/dcc_viss.bin "$OUT_DIR"/dcc_2a.bin "$OUT_DIR"/dcc_ldc.bin
rm -f "$DCC_XML_DIR"/*.bin

cd "$DCC_XML_DIR"

echo "[INFO] DCC tool: $DCC_TOOL_PATH"
echo "[INFO] XML source: $DCC_XML_DIR"

echo "[INFO] Generating VISS components..."
"$DCC_TOOL_PATH/dcc_gen_linux" imx296_rgb2rgb_dcc.xml
"$DCC_TOOL_PATH/dcc_gen_linux" imx296_h3a_aewb_dcc.xml
"$DCC_TOOL_PATH/dcc_gen_linux" imx296_viss_nsf4.xml
"$DCC_TOOL_PATH/dcc_gen_linux" imx296_viss_blc.xml
"$DCC_TOOL_PATH/dcc_gen_linux" imx296_cfa_dcc.xml
"$DCC_TOOL_PATH/dcc_gen_linux" imx296_viss_gamma_dcc.xml
"$DCC_TOOL_PATH/dcc_gen_linux" imx296_linear_decompand_dcc.xml
"$DCC_TOOL_PATH/dcc_gen_linux" imx296_h3a_mux_luts_dcc.xml
cat *.bin > "$OUT_DIR/dcc_viss.bin"

rm -f ./*.bin

echo "[INFO] Generating 2A components..."
"$DCC_TOOL_PATH/dcc_gen_linux" imx296_awb_alg_ti3_tuning.xml
"$DCC_TOOL_PATH/dcc_gen_linux" imx296_h3a_aewb_dcc.xml
cat *.bin > "$OUT_DIR/dcc_2a.bin"

rm -f ./*.bin

echo "[INFO] Generating LDC components..."
"$DCC_TOOL_PATH/dcc_gen_linux" imx296_mesh_ldc_dcc.xml
cat *.bin > "$OUT_DIR/dcc_ldc.bin"

rm -f ./*.bin

echo "[INFO] Generated DCC files:"
ls -lh "$OUT_DIR"/dcc_*.bin

echo "[INFO] MD5 checksums:"
md5sum "$OUT_DIR"/dcc_*.bin
