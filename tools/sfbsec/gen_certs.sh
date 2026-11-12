#!/bin/sh
# Generate the SFB security certificate classes.
#
#   gen_certs.sh <output-dir>
#
# Produces:
#   auth.key / auth.cer   class A - fastboot authorization (SSH-style
#                         challenge signing key; keep the key offline)
#   boot.key / boot.cer   class B - EFI code signing
#
# Copy the .cer files into the efisp config container:
#   SECURITY/AUTH_CERTS/*.cer   (auth)
#   SECURITY/BOOT_CERTS/*.cer   (boot)
#
# X.509 DER (.cer), RSA 2048. The BDS parses certificates with mbedtls.
set -e
out="${1:-.}"
mkdir -p "$out"

openssl genrsa -out "$out/auth.key" 2048
openssl req -new -x509 -key "$out/auth.key" -out "$out/auth.cer" \
  -outform DER -days 3650 -subj "/CN=sfb auth (fastboot unlock)"

openssl genrsa -out "$out/boot.key" 2048
openssl req -new -x509 -key "$out/boot.key" -out "$out/boot.cer" \
  -outform DER -days 3650 -subj "/CN=sfb boot (EFI signing)"

echo "Certificates written to $out"
echo "  AUTH_CERTS <- $out/auth.cer   (locks fastboot + Advanced until challenged)"
echo "  BOOT_CERTS <- $out/boot.cer   (enables EFI image verification)"
echo "Keep the .key files private; the device never needs them."
