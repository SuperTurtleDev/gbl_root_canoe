#!/bin/bash
# Host-side build & run for the SfbMemFixupLib unit tests (no EDK2, plain cc):
#   MemFixupTest  - TZ merge interval arithmetic (+ real-device regression)
#   MemCfgTest    - CONFIG\MEMCFG parser and override application
# Pass memmap.txt tzmap.txt to run the regression against other dumps.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
T="${TMPDIR:-/tmp}"
CFLAGS="-DSFB_HOST_TEST -O1 -g -Wall -Wextra -Wno-unused-parameter -I$HERE/.."

cc $CFLAGS -o "$T/MemFixupTest" "$HERE/MemFixupTest.c" "$HERE/../SfbMemRegions.c"
cc $CFLAGS -o "$T/MemCfgTest"   "$HERE/MemCfgTest.c"   "$HERE/../SfbMemCfg.c"

rc=0
if [ $# -ge 2 ]; then
  "$T/MemFixupTest" "$1" "$2" || rc=1
else
  "$T/MemFixupTest" "$HERE/fixtures/memmap.txt" "$HERE/fixtures/tzmap.txt" || rc=1
fi
"$T/MemCfgTest" || rc=1
exit $rc
