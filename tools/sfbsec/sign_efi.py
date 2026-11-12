#!/usr/bin/env python3
"""Sign an EFI image for the SFB BOOT_CERTS verification.

Usage: sign_efi.py <boot.cer> <boot.key> <app.efi> [output.efi]

The signature is a PKCS#7 (no signed attributes, SHA-256, via openssl smime)
over the byte stream the BDS authentihash covers - everything in the file
except the COFF checksum field, the security data-directory entry and the
certificate table itself. It is appended as a WIN_CERTIFICATE entry and the
security directory is patched to point at it. Mirrors SfbPeVerify.c in the
BDS.
"""

import os
import struct
import subprocess
import sys
import tempfile

PE_DIR_SECURITY = 4


def pe_layout(blob):
    pe_off = struct.unpack_from("<I", blob, 0x3c)[0]
    if blob[pe_off:pe_off + 4] != b"PE\0\0":
        raise SystemExit("not a PE image")
    opt_off = pe_off + 4 + 20
    opt_magic = struct.unpack_from("<H", blob, opt_off)[0]
    if opt_magic == 0x010b:        # PE32
        data_dir_off = opt_off + 96
    elif opt_magic == 0x020b:      # PE32+
        data_dir_off = opt_off + 112
    else:
        raise SystemExit("bad optional header magic 0x%x" % opt_magic)
    checksum_off = opt_off + 64
    sec_dir_off = data_dir_off + PE_DIR_SECURITY * 8
    size_of_headers = struct.unpack_from("<I", blob, opt_off + 60)[0]
    return checksum_off, sec_dir_off, size_of_headers


def canonical_stream(blob):
    checksum_off, sec_dir_off, size_of_headers = pe_layout(blob)
    sd_off, sd_size = struct.unpack_from("<II", blob, sec_dir_off)
    end = len(blob)
    if sd_size:
        end = sd_off
    return (blob[:checksum_off]
            + blob[checksum_off + 4:sec_dir_off]
            + b"\0" * 8
            + blob[sec_dir_off + 8:size_of_headers]
            + blob[size_of_headers:end])


def main():
    if len(sys.argv) not in (4, 5):
        print(__doc__)
        return 2
    cert, key, app = sys.argv[1:4]
    out = sys.argv[4] if len(sys.argv) == 5 else app + ".signed"

    with open(app, "rb") as f:
        blob = bytearray(f.read())
    checksum_off, sec_dir_off, size_of_headers = pe_layout(blob)

    with tempfile.TemporaryDirectory() as tmp:
        canon = os.path.join(tmp, "canonical.bin")
        with open(canon, "wb") as f:
            f.write(canonical_stream(blob))
        sig = os.path.join(tmp, "sig.p7")
        subprocess.run(
            ["openssl", "smime", "-sign", "-noattr", "-binary",
             "-md", "sha256", "-in", canon, "-signer", cert,
             "-inkey", key, "-outform", "DER", "-out", sig], check=True)
        with open(sig, "rb") as f:
            pkcs7 = f.read()

    # WIN_CERTIFICATE (wLength, wRevision 0x0200, type 0x0002 PKCS_SIGNED_DATA)
    # + the DER body, padded to an 8-byte multiple.
    entry = struct.pack("<HHHH", 8 + len(pkcs7), 0x0200, 0x0002, 0) + pkcs7
    entry += b"\0" * ((-len(entry)) % 8)

    cert_off = len(blob)
    struct.pack_into("<II", blob, sec_dir_off, cert_off, len(entry))
    with open(out, "wb") as f:
        f.write(bytes(blob) + entry)
    print("signed %s -> %s (cert table at %d, %d bytes)"
          % (app, out, cert_off, len(entry)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
