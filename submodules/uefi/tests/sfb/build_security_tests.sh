#!/bin/sh
# Build the host security round-trip test against the in-tree MdePkg headers
# and the real mbedtls sources with the trimmed SFB config.
# Usage: build_security_tests.sh <output-binary>
set -e
here=$(cd "$(dirname "$0")" && pwd)
edk2="$here/../../edk2"
sec="$edk2/QcomModulePkg/Library/SfbSecurityLib"
mb="$edk2/CryptoPkg/Library/MbedTlsLib"
case "$(uname -m)" in
  x86_64|amd64) arch=X64 ;;
  aarch64|arm64) arch=AArch64 ;;
  *) echo "unsupported host $(uname -m)" >&2; exit 2 ;;
esac

MBED_SRC="asn1parse asn1write base64 bignum bignum_core constant_time md \
oid ecp ecdsa ecp_curves pem pk pk_ecc pk_wrap pkparse pkcs7 platform_util rsa rsa_alt_helpers \
sha256 sha512 x509 x509_create x509_crt x509_crl"

srcs=""
for f in $MBED_SRC; do
  srcs="$srcs $mb/mbedtls/library/$f.c"
done

shim=$(mktemp -d)/mbedtls_host_shim.c
trap 'rm -rf "$(dirname "$shim")"' EXIT
cat > "$shim" <<EOF
#include <stdlib.h>
void *mbedtls_calloc(size_t n, size_t s) { return calloc(n, s); }
void mbedtls_free(void *p) { free(p); }
long long mbedtls_ms_time(void) { return 0; }
#include <stdarg.h>
#include <stdio.h>
int my_snprintf(char *b, size_t n, const char *fmt, ...) { va_list a; va_start(a, fmt); int r = vsnprintf(b, n, fmt, a); va_end(a); return r; }
EOF

${CC:-cc} -std=gnu11 -fshort-wchar -fno-strict-aliasing -DMDEPKG_NDEBUG \
  -Wall -Wno-unused-function -Wno-sign-compare -g \
  ${SFB_SANITIZE--fsanitize=address,undefined} \
  -I"$edk2/MdePkg/Include" -I"$edk2/MdePkg/Include/$arch" \
  -I"$edk2/QcomModulePkg/Include" -I"$sec" \
  -I"$mb/Include" -I"$mb/mbedtls/include" -I"$mb/mbedtls/library" \
  "$here/host/test_sfb_security.c" \
  "$sec/SfbSecurity.c" "$sec/SfbPeVerify.c" \
  $srcs "$shim" \
  -o "$1"
