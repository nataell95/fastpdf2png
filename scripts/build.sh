#!/bin/bash
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(dirname "$SCRIPT_DIR")"
cd "$ROOT"

# Download PDFium if needed
if [ ! -d "pdfium" ]; then
    echo "Downloading PDFium..."
    "$SCRIPT_DIR/get_pdfium.sh"
fi

PRESET="${FP2P_PRESET:-release}"
echo "=== Building fastpdf2png (cmake --preset $PRESET) ==="

cmake --preset "$PRESET"
cmake --build build --parallel

echo ""
echo "=== Build complete ==="
echo "Binary: $ROOT/build/fastpdf2png"
