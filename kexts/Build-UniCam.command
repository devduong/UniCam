#!/usr/bin/env bash
set -euo pipefail

KEXTS_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "${KEXTS_DIR}"

echo "========================================================================"
echo "  UniCam.kext Builder (1-Click Pure-EFI Webcam Kext for macOS)"
echo "========================================================================"
echo ""

if ! xcrun --find clang++ >/dev/null 2>&1; then
    echo "[!] Xcode Command Line Tools are not installed (missing clang++ compiler)."
    echo "    Opening Apple's installer dialog — click Install, wait for completion, then re-run this script."
    xcode-select --install 2>/dev/null || true
    exit 1
fi

if ! command -v python3 >/dev/null 2>&1; then
    echo "[!] python3 is missing. Please install Xcode Command Line Tools or Python 3 and re-run."
    exit 1
fi

echo "[*] Downloading/verifying Intel IPU4P firmware and building UniCam.kext..."
make -C UniCam verify

OUT_KEXT="${KEXTS_DIR}/UniCam/build/UniCam.kext"
echo ""
echo "========================================================================"
echo "  [DONE] Successfully built:"
echo "    ${OUT_KEXT}"
echo ""
echo "  Next steps:"
echo "   1. Copy UniCam.kext into EFI/OC/Kexts/ on your OpenCore EFI partition."
echo "   2. In config.plist -> Kernel -> Add: enable UniCam.kext AFTER VoodooI2C.kext."
echo "   3. Enable SSDT-CAM-<Board>.aml in ACPI -> Add, then Reboot."
echo "========================================================================"

if [[ -t 0 && -z "${UNICAM_NONINTERACTIVE:-}" ]]; then
    open -R "${OUT_KEXT}" 2>/dev/null || true
fi
