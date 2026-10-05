# GBL Root Canoe

[中文版](README_zh.md)

![Stone Badge](https://stone.professorlee.work/api/stone/superturtlee/gbl_root_canoe)

`gbl_root_canoe` is an EDK2-based workspace for patching the EFI applications within Qualcomm ABL (Android Bootloader) images. It leverages a GBL (Generic Bootloader Loader) vulnerability so the real ABL loads an embedded **superfastboot BDS** off the raw `efisp` partition. The BDS then scans a compatible partition (ext4/fat32) for boot entries and chains to the selected one - primarily to achieve a **Fake Locked Bootloader** state on Snapdragon 8 Gen 5 / 8 Elite (Gen 5) devices to bypass bootloader unlock detection.

`BDS.efi` is written raw to the `efisp` partition; the cracked ABL (`boot.efi`) and the boot entry list (`BOOTENTRIES`) live on the `persist` partition under its `efisp/` directory.

---

## Builder Guide

This section is for developers who want to compile the toolkits from source.

### Prerequisites
You must be on a **Linux** host to build the project:
- `gcc` / `clang`, `lld`, `make`, `zip`, `python3`
- `liblzma-dev` (for compiling `extractfv`)
- **Android NDK** (Required for `make target_magisk_module` to cross-compile tools for Android)
- **MinGW-w64**

### Build Targets

**Note:** You **do not** need to provide an `abl.img` to build the distributable toolkits or module.

- **`make target_toolkit_linux`**
  Builds the superfastboot BDS (`BDS.efi`) from the `uefi` submodule and compiles the patching utilities (`extractfv`, `patch_abl`) for Linux.

- **`make target_toolkit_windows`**
  Same as above, but cross-compiles the utilities into Windows `.exe` programs using MinGW-w64.

- **`make target_magisk_module`**
  Cross-compiles the patcher tools for Android using your NDK, builds the BDS, and packages everything as a KernelSU/Magisk module.

- **`make target_toolkit_android`**
  Produces a standalone Android arm64 toolkit (`toolkit_android.zip`) with Android-native binaries for on-device use outside of the module.

---

## User Guide

For more detailed instructions, please refer to the [Wiki](https://github.com/superturtlee/gbl_root_canoe/wiki).

### 1. Using the Module (On-Device)

The module is designed to run directly on your rooted Android device.

**Requirements:**
- Device must be Snapdragon 8 Gen 5 / 8 Elite (Gen 5).
- Bootloader must be unlocked.
- Kernel must NOT have Baseband Guard.
- The ABL on the `abl` partition must contain the GBL vulnerability. If it does not, flash an older ABL with the vulnerability first (the cracked `boot.efi` does not need to match the ABL on the `abl` partition).

**Installation & Usage:**
When flashing the module via a root manager (KernelSU, Magisk, or APatch), the script interacts with you using the volume keys:
- **Volume Up (First-time installation):** The script extracts the current-slot `.abl`, cracks it into `boot.efi`, places `boot.efi` / `LinuxLoader.efi` / `BOOTENTRIES` into `/mnt/vendor/persist/efisp/`, and flashes `BDS.efi` to `efisp`. After this, reboot into Recovery and **format Data**. Once booted, install this module again (Volume Down the second time) to complete the installation.
- **Volume Down (OTA retention or post-format):** Installs the OTA-update patch. After each OTA, open the module WebUI and flash again to retain the BL version.

### 2. Using the PC Toolkits (Linux / Windows)

If you downloaded the `target_toolkit_linux` or `target_toolkit_windows` zip files:
1. Extract the toolkit zip on your PC.
2. Place your device's stock `abl.img` inside the `images/` (or `images\`) directory of the toolkit.
3. **Linux:** Run `bash build.sh`. **Windows:** Run `build.bat`.
4. The script extracts and cracks the ABL, outputting `ABL.efi` (fake re-lock) and `ABL_original.efi` (original). `BDS.efi` is bundled. Check `patch_log.txt` - if it says "Warning: Failed to patch ABL GBL", the ABL lacks the vulnerability and the `abl` partition must be downgraded to an older ABL with it.

For Xiaomi/HyperOS region override, explicitly run `bash build.sh --hwcountry-global` (Linux), `build.bat --hwcountry-global` (Windows), or `sh build.sh --hwcountry-global` (Android toolkit). This redirects the supported ABL's HwCountry getvar and display references to `GLOBAL`, adapting the feature from [HyperCanoe](https://github.com/StevenWin818/GBL_Root_HyperCanoe). The default build keeps the hardware country unchanged. Unsupported or ambiguous layouts and unavailable PE tail padding fail without producing a patched file. The patcher also accepts `patch_abl ABL_original.efi boot.efi --hwcountry-global` directly; the module's automatic installer does not enable this option.

Then complete the install manually (see the [Wiki](https://github.com/superturtlee/gbl_root_canoe/wiki) for full steps): copy the generated `efisp/boot.efi` into `/mnt/vendor/persist/efisp/`, use the toolkit's `BOOTENTRIES`, `sync`, and flash `BDS.efi` to `efisp` (`dd if=BDS.efi of=/dev/block/by-name/efisp bs=4M`). `boot.efi` is a chain-loaded EFI application: do not flash it to the `abl` or `efisp` partition. Static patch validation does not establish that a device will boot; the installed ABL/BDS chain must be compatible.

### 3. OTA Upgrade
Before rebooting for an OTA update, use the module WebUI to flash and retain the old ABL version. "Update efisp" is enabled by default; for a major version upgrade keep it on, otherwise the device may get stuck on the first boot screen.

### 4. Superfastboot Usage Instructions
When OEM Unlocking is enabled and the white warning text appears on boot, press **Volume Down** to enter Superfastboot mode (the BDS).
Common commands include:
- **Temp-boot an EFI file (without flashing)**: `fastboot boot xxx.efi`
- **Lock and Unlock (BL related)**:
  - Lock BL, triggers a data wipe: `fastboot flashing lock`
  - Unlock BL, no data wipe: `fastboot flashing unlock` or `fastboot flashing unlock_critical`
  - *Note: If the TEE status is inconsistent, the device will refuse to provide the data key, rendering data inaccessible.*
- **Flashing and Erasing**:
  - `fastboot flash <partition> <file.img>`
  - `fastboot erase <partition>`
- **Rebooting**:
  - `fastboot reboot bootloader` (Next normal boot enters Official Fastboot)
  - `fastboot reboot recovery`
  - `fastboot reboot`

### 5. File Reference
1. `BDS.efi`: The superfastboot BDS, flashed raw to the `efisp` partition.
2. `boot.efi` / `ABL.efi`: The cracked ABL with fake re-lock (the module names it `boot.efi`; the toolkit names it `ABL.efi`), placed on `persist` under `efisp/`.
3. `LinuxLoader.efi` / `ABL_original.efi`: The original unpatched ABL. For analysis; do not flash to `efisp`.
4. `BOOTENTRIES`: Boot entry list, format `<name>:<path relative to efisp/>`.
