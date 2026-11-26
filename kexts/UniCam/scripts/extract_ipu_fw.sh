#!/usr/bin/env bash
set -euo pipefail

# extract_ipu_fw.sh
# Extract Intel IPU4P (8086:8a19) firmware for embedded/deferred loading
# Source 1 (--windows-dir): read-only scan of Windows DriverStore/FileRepository
# Source 2 (--download): download official CAB package from Microsoft Update Catalog (42.18362.3.16827)

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KEXT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
OUT_DIR="${KEXT_DIR}/firmware/ipu4p"

EXPECTED_SHA256="ff2c36cc81a5c726508b22970c2e2538ff06107dc5a72c93401403c227e5157f"
CAB_URL="https://catalog.s.download.windowsupdate.com/c/msdownload/update/driver/drvs/2025/01/dba9022d-a496-4f09-b989-4815f8e83643_32fc2322514685244f80f83e323b794e93621066.cab"

MODE="auto"
WIN_DIR=""

usage() {
    cat <<EOF
Usage: $(basename "$0") [--windows-dir <path> | --download] [--out-dir <path>]

Options:
  --windows-dir <path>  Scan Windows DriverStore directory (read-only) for cpd_component_signed.bin
  --download            Download and extract official Intel Camera Driver CAB 42.18362.3.16827
  --out-dir <path>      Output directory (default: ${OUT_DIR})
  -h, --help            Show help
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --windows-dir)
            MODE="windows"
            WIN_DIR="${2:-}"
            shift 2
            ;;
        --download)
            MODE="download"
            shift
            ;;
        --out-dir)
            OUT_DIR="${2:-}"
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "[!] Invalid argument: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

mkdir -p "${OUT_DIR}"
TARGET_BIN="${OUT_DIR}/cpd_component_signed.bin"
ALIAS_BIN="${OUT_DIR}/ipu4p_fw.bin"
SUM_FILE="${OUT_DIR}/SHA256SUMS.txt"

verify_sha256() {
    local file_path="$1"
    local actual
    actual="$(shasum -a 256 "${file_path}" | awk '{print $1}')"
    if [[ "${actual}" != "${EXPECTED_SHA256}" ]]; then
        echo "[!] SHA-256 mismatch error!" >&2
        echo "    Expected: ${EXPECTED_SHA256}" >&2
        echo "    Actual  : ${actual}" >&2
        return 1
    fi
    return 0
}

# Quick verification if a valid binary already exists in auto mode
if [[ "${MODE}" == "auto" && -f "${TARGET_BIN}" ]]; then
    if verify_sha256 "${TARGET_BIN}"; then
        ln -sf "cpd_component_signed.bin" "${ALIAS_BIN}"
        echo "${EXPECTED_SHA256}  cpd_component_signed.bin" > "${SUM_FILE}"
        echo "[+] IPU4P firmware is ready and SHA-256 verified: ${TARGET_BIN}"
        exit 0
    fi
fi

# Extract from read-only Windows directory
if [[ "${MODE}" == "windows" ]]; then
    if [[ -z "${WIN_DIR}" || ! -d "${WIN_DIR}" ]]; then
        echo "[!] Windows directory not found: ${WIN_DIR}" >&2
        exit 1
    fi
    FOUND_FW="$(find "${WIN_DIR}" -type f \( -iname "cpd_component_signed.bin" -o -iname "irci_irci_ecr-master_*.bin" \) 2>/dev/null | head -n 1 || true)"
    if [[ -z "${FOUND_FW}" ]]; then
        echo "[!] cpd_component_signed.bin not found in ${WIN_DIR}" >&2
        exit 1
    fi
    echo "[*] Found source firmware: ${FOUND_FW}"
    cp "${FOUND_FW}" "${TARGET_BIN}"
else
    TMP_DIR="$(mktemp -d /tmp/unicam_fw.XXXXXX)"
    trap 'rm -rf "${TMP_DIR}"' EXIT
    CAB_FILE="${TMP_DIR}/intel_ipu4p_driver.cab"
    EXTRACT_DIR="${TMP_DIR}/extracted"
    mkdir -p "${EXTRACT_DIR}"

    echo "[*] Downloading Intel Camera Driver package (42.18362.3.16827)..."
    curl -fL --retry 3 --connect-timeout 15 -o "${CAB_FILE}" "${CAB_URL}"

    echo "[*] Extracting CAB archive..."
    if command -v cabextract >/dev/null 2>&1; then
        cabextract -q -d "${EXTRACT_DIR}" -F "*cpd_component_signed.bin*" "${CAB_FILE}" || cabextract -q -d "${EXTRACT_DIR}" "${CAB_FILE}"
    elif command -v 7z >/dev/null 2>&1; then
        7z e -y "-o${EXTRACT_DIR}" "${CAB_FILE}" "*cpd_component_signed.bin*" >/dev/null
    elif command -v 7zz >/dev/null 2>&1; then
        7zz e -y "-o${EXTRACT_DIR}" "${CAB_FILE}" "*cpd_component_signed.bin*" >/dev/null
    else
        bsdtar -xf "${CAB_FILE}" -C "${EXTRACT_DIR}" cpd_component_signed.bin 2>/dev/null || bsdtar -xf "${CAB_FILE}" -C "${EXTRACT_DIR}" 2>/dev/null || true
    fi

    FOUND_FW="$(find "${EXTRACT_DIR}" -type f -iname "cpd_component_signed.bin" | head -n 1 || true)"
    if [[ -z "${FOUND_FW}" ]]; then
        echo "[!] cpd_component_signed.bin not found after extracting CAB." >&2
        exit 1
    fi
    cp "${FOUND_FW}" "${TARGET_BIN}"
fi

chmod 0644 "${TARGET_BIN}"
verify_sha256 "${TARGET_BIN}"
ln -sf "cpd_component_signed.bin" "${ALIAS_BIN}"
{
    echo "# Intel IPU4P (8086:8a19) Signed Firmware — Package 42.18362.3.16827"
    echo "${EXPECTED_SHA256}  cpd_component_signed.bin"
} > "${SUM_FILE}"

SIZE_BYTES="$(stat -f%z "${TARGET_BIN}")"
echo "[+] Extracted firmware : ${TARGET_BIN} (${SIZE_BYTES} bytes)"
echo "[+] SHA-256 Verified   : ${EXPECTED_SHA256}"
echo "[+] Wrote checksum to  : ${SUM_FILE}"
