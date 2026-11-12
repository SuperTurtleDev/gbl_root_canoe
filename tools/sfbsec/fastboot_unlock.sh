#!/bin/sh
# Unlock a locked SFB BDS over fastboot, SSH-style.
#
#   fastboot_unlock.sh <auth.key>
#
# Flow (one attempt per boot; the nonce is consumed even on failure):
#   fastboot fetch sb_challenge challenge.bin
#   openssl dgst -sha256 -sign <auth.key> challenge.bin > sb_challenge.sig
#   fastboot flash sb_challenge.sig sb_challenge.sig
#
# The firmware publishes partition-size:sb_challenge, so the stock Android
# fastboot client (36.x) accepts the virtual partition.
set -e
key="${1:?usage: fastboot_unlock.sh <auth.key>}"
dir="$(mktemp -d)"
trap 'rm -rf "$dir"' EXIT

fastboot fetch sb_challenge "$dir/challenge.bin"
openssl dgst -sha256 -sign "$key" \
  -out "$dir/sb_challenge.sig" "$dir/challenge.bin"
fastboot flash sb_challenge.sig "$dir/sb_challenge.sig"
echo "Unlocked for this boot."
