# Intel IPU4P Firmware (`8086:8a19`)

This directory contains the SHA-256 verification metadata and staging location for the **Intel Ice Lake IPU4P** (`PCI 0:5:0`, Device ID `8086:8a19`, Revision `0x03`) image signal processor firmware.

> [!NOTE]
> Proprietary `*.bin` / `*.cab` binaries are excluded via `.gitignore` and **never** committed to Git. Running `Build-UniCam.command` or `make` downloads and verifies them automatically via [`../scripts/extract_ipu_fw.sh`](../scripts/extract_ipu_fw.sh).

## Expected Firmware Metadata

- **Primary binary:** `kexts/UniCam/firmware/ipu4p/cpd_component_signed.bin` (symlink `ipu4p_fw.bin`)
- **Intel Camera Driver package version:** `42.18362.3.16827` (Microsoft Update Catalog)
- **Expected SHA-256:**
  ```text
  ff2c36cc81a5c726508b22970c2e2538ff06107dc5a72c93401403c227e5157f  cpd_component_signed.bin
  ```

## Manual Extraction

### Method 1: Download & extract automatically from Microsoft Update Catalog (Recommended on macOS)
```bash
./kexts/UniCam/scripts/extract_ipu_fw.sh --download
```

### Method 2: Read-only extraction from a Windows DriverStore backup
If `C:\Windows\System32\DriverStore\FileRepository` was copied to `/Volumes/New Volume`:
```bash
./kexts/UniCam/scripts/extract_ipu_fw.sh --windows-dir "/Volumes/New Volume/FileRepository"
```

## Pure-EFI In-Kext Embedding

During the `UniCam.kext` build, [`../scripts/gen_embedded_fw.py`](../scripts/gen_embedded_fw.py) compresses `cpd_component_signed.bin` with `zlib -9` (`~948 KB`) and compiles it into `UniCam.kext`. At runtime, the kext inflates the firmware directly in RAM using XNU's built-in `zlib` (`_inflateInit2_` / `_inflate`) — **requiring zero files installed on macOS and zero background daemons**.
