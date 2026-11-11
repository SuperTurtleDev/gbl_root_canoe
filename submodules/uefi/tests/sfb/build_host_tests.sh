#!/bin/sh
# Build the host unit tests against the in-tree MdePkg headers.
# Usage: build_host_tests.sh <output-binary>
set -e
here=$(cd "$(dirname "$0")" && pwd)
edk2="$here/../../edk2"
lld="$edk2/QcomModulePkg/Application/LinuxLoader"
store="$edk2/QcomModulePkg/Library/SfbConfigStoreLib"
case "$(uname -m)" in
  x86_64|amd64) arch=X64 ;;
  aarch64|arm64) arch=AArch64 ;;
  *) echo "unsupported host $(uname -m)" >&2; exit 2 ;;
esac
${CC:-cc} -std=gnu11 -fshort-wchar -fno-strict-aliasing -DMDEPKG_NDEBUG \
  -Wall -Wno-unused-function -g ${SFB_SANITIZE:--fsanitize=address,undefined} \
  -I"$edk2/MdePkg/Include" -I"$edk2/MdePkg/Include/$arch" \
  -I"$edk2/QcomModulePkg/Include" -I"$lld" -I"$store" \
  "$here/host/test_sfb.c" "$here/host/edk2_stubs.c" \
  "$lld/SuperFbEntryList.c" "$lld/SuperFbSynthDisk.c" \
  "$store/SuperFbConfigDisk.c" "$store/SuperFbConfigFormat.c" \
  -o "$1"
