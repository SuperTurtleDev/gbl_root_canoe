#!/bin/bash
# Host-side build & run for MemFixupTest (no EDK2, no container).
# Runs the real-device regression against the fixtures by default.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="${TMPDIR:-/tmp}/MemFixupTest"
cc -DSFB_HOST_TEST -O1 -g -Wall -Wextra -Wno-unused-parameter \
   -I"$HERE/.." -o "$OUT" "$HERE/MemFixupTest.c" "$HERE/../SfbMemRegions.c"
if [ $# -ge 2 ]; then
  "$OUT" "$1" "$2"
else
  "$OUT" "$HERE/fixtures/memmap.txt" "$HERE/fixtures/tzmap.txt"
fi
