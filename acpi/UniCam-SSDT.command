#!/usr/bin/env bash
set -euo pipefail

ACPI_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "${ACPI_DIR}"

if ! command -v python3 >/dev/null 2>&1; then
    echo "[!] python3 is not installed. Please install Xcode Command Line Tools (xcode-select --install) and try again."
    exit 1
fi

exec python3 "${ACPI_DIR}/scripts/unicam_ssdt_wizard.py" "$@"
