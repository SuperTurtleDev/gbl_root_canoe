#!/bin/sh
# Refresh app/src/main/assets from this repository's build outputs.
#
# Requires NDK_PATH (aarch64-linux-android31-clang), matching the toolkit and
# magisk-module builds. From the repository root:
#
#   NDK_PATH=$ANDROID_NDK_LATEST_HOME sh targets/android_app/refresh_assets.sh
#
set -e
root=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
assets="$root/targets/android_app/app/src/main/assets"
mkdir -p "$assets"

# BDS.efi (always rebuildable with the project Makefile).
make -C "$root/submodules/uefi" build
cp "$root/submodules/uefi/build/BDS.efi" "$assets/BDS.efi"

# secure_fastboot.efi: the system-loader wrapper (separate package).
make -C "$root/submodules/uefi" securefastboot
cp "$root/submodules/uefi/build/SecureFastboot.efi" "$assets/secure_fastboot.efi"

# patch_abl / extractfv: same NDK targets the magisk module uses.
: "${NDK_PATH:?set NDK_PATH to your Android NDK (aarch64-linux-android31-clang)}"
make -C "$root/submodules/patcher" build_android
make -C "$root/submodules/ablfvextractor" build_android
cp "$root/submodules/patcher/build/patch_abl_android" "$assets/patch_abl"
cp "$root/submodules/ablfvextractor/build/extractfv_android" "$assets/extractfv"

echo "assets refreshed:"
ls -la "$assets"
