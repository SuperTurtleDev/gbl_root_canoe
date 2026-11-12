#!/usr/bin/env python3
"""Security round-trip driver: generate the key material and the signed
synthetic PE, then run the compiled host test that exercises the real
SfbSecurityLib verification code.

Everything lands in one temp dir:

  auth.key / auth.cer    class A key pair (fastboot authorization)
  boot.key / boot.cer    class B key pair (EFI code signing)
  nonce.bin              minted by the test binary
  challenge.sig          openssl signature over nonce.bin with auth.key
  challenge-bad.sig      signature over a different nonce, must fail
  canonical.bin          the byte ranges the authentihash covers
  sig.p7                 PKCS#7 over canonical.bin, boot.key, -noattr
  app-signed.efi         synthetic PE + WIN_CERTIFICATE-wrapped sig.p7

Run directly for a standalone pass, or via tests/sfb/test_sfb.py.
"""

import os
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))

CHALLENGE_SIZE = 32


def run(cmd, **kw):
    subprocess.run(cmd, check=True, **kw)


def make_key(dirpath, name):
    key = os.path.join(dirpath, name + ".key")
    crt = os.path.join(dirpath, name + ".cer")
    run(["openssl", "genrsa", "-out", key, "2048"])
    run(["openssl", "req", "-new", "-x509", "-key", key, "-out", crt,
         "-outform", "DER", "-days", "3650",
         "-subj", "/CN=sfb-test-" + name])
    return key, crt


def build_pe(dirpath, boot_key, boot_crt):
    """A minimal PE32+ with enough header room for the 16 data directories,
    a small body and an appended certificate table."""
    pe_off = 0x40
    dos = bytearray(b"MZ" + b"\0" * 58 + struct.pack("<I", pe_off))
    pe_sig = b"PE\0\0"
    coff = struct.pack("<HHIIIHH", 0x8664, 0, 0, 0, 0, 240, 0x0022)
    opt_magic = 0x020b
    opt = bytearray(struct.pack("<HBB", opt_magic, 14, 0))  # magic, lnk ver
    opt += b"\0" * (60 - len(opt))                          # pad to SizeOfHeaders
    opt += struct.pack("<I", 0x400)                         # SizeOfHeaders
    opt += struct.pack("<I", 0)                             # CheckSum
    opt += b"\0" * (68 - len(opt))                          # pad to Subsystem
    opt += struct.pack("<H", 0x000B)                        # subsystem EFI app
    opt += b"\0" * (112 - len(opt))                         # pad to data dirs
    opt += b"\0" * (16 * 8)                                 # 16 data dirs
    headers = dos + pe_sig + coff + bytes(opt)
    headers += b"\0" * (0x400 - len(headers))
    body = bytes(range(256)) * 64                           # 16 KiB section data

    checksum_off = pe_off + 4 + 20 + 64
    secdir_off = pe_off + 4 + 20 + 112 + 4 * 8
    size_of_headers = 0x400

    # The byte stream the device-side authentihash covers: everything except
    # the CheckSum field, the security data-directory entry and the cert table.
    blob = bytearray(headers + body)
    canonical = (
        bytes(blob[:checksum_off])
        + bytes(blob[checksum_off + 4:secdir_off])
        + b"\0" * 8
        + bytes(blob[secdir_off + 8:size_of_headers])
        + bytes(blob[size_of_headers:])
    )
    canon_path = os.path.join(dirpath, "canonical.bin")
    with open(canon_path, "wb") as f:
        f.write(canonical)

    sig_path = os.path.join(dirpath, "sig.p7")
    run(["openssl", "smime", "-sign", "-noattr", "-binary", "-md", "sha256",
         "-in", canon_path, "-signer", boot_crt, "-inkey", boot_key,
         "-outform", "DER", "-out", sig_path])
    with open(sig_path, "rb") as f:
        pkcs7 = f.read()

    cert_off = len(blob)
    # WIN_CERTIFICATE: wLength, wRevision 0x0200, wCertificateType 0x0002
    # (PKCS_SIGNED_DATA), reserved.
    cert = struct.pack("<HHHH", 8 + len(pkcs7), 0x0200, 0x0002, 0) + pkcs7
    cert += b"\0" * ((-len(cert)) % 8)
    entry = cert
    blob[secdir_off:secdir_off + 8] = struct.pack("<II", cert_off,
                                                  len(entry))
    out = os.path.join(dirpath, "app-signed.efi")
    with open(out, "wb") as f:
        f.write(bytes(blob) + entry)
    return out


def main():
    art = os.environ.get("SFB_TEST_DIR") or tempfile.mkdtemp(
        prefix="sfb-sec-")
    os.makedirs(art, exist_ok=True)

    auth_key, auth_crt = make_key(art, "auth")
    boot_key, boot_crt = make_key(art, "boot")
    build_pe(art, boot_key, boot_crt)

    exe = os.path.join(art, "test_sfb_security")
    # The test binary intentionally keeps pools and buffers until exit; the
    # ASAN default leak report would turn that into a failure.
    os.environ.setdefault("ASAN_OPTIONS", "detect_leaks=0")
    run([os.path.join(HERE, "build_security_tests.sh"), exe])

    run([exe, art, "mint"])

    nonce = os.path.join(art, "nonce.bin")
    run(["openssl", "dgst", "-sha256", "-sign", auth_key,
         "-out", os.path.join(art, "challenge.sig"), nonce])

    # A signature over a different nonce: valid RSA, wrong message.
    with open(os.path.join(art, "nonce-other.bin"), "wb") as f:
        f.write(b"\x5a" * CHALLENGE_SIZE)
    run(["openssl", "dgst", "-sha256", "-sign", auth_key,
         "-out", os.path.join(art, "challenge-bad.sig"),
         os.path.join(art, "nonce-other.bin")])

    return subprocess.run([exe, art, "verify"]).returncode


if __name__ == "__main__":
    sys.exit(main())
