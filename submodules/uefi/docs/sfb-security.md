# SFB security model (SECURITY certificates, fastboot lock, EFI verification)

The BDS is the root of trust. Two certificate classes live in the efisp
config container as X.509 DER files, classified by directory (not by
certificate contents):

- `AUTH_CERTS\*.cer` - **class A**, fastboot authorization
- `BOOT_CERTS\*.cer` - **class B**, EFI code signing

Everything is certificate-driven, and the two classes are **independent**:
privilege never exempts an image from BOOT_CERTS verification.

Boot flow: the device starts **non-privileged**. The BOOT_CERTS list loads
first; an empty list means no signature verification. Before the boot menu
runs, the AUTH_CERTS list loads; when it is empty there is nothing to
authorize against and privilege is granted directly (fully open menus -
also the previous behavior). With AUTH_CERTS present the device stays
locked until a challenge response succeeds.

| AUTH_CERTS | BOOT_CERTS | Behavior |
| --- | --- | --- |
| empty | empty | Fully open (previous behavior) |
| non-empty | empty | Locked privileged operations until challenged; no image verification |
| empty | non-empty | Open menus; signed EFI enforcement always |
| non-empty | non-empty | Locked privileged operations + signed EFI enforcement always |

Certificates are loaded once at boot, while the container is mounted, and
kept in memory - fastboot runs with the store unmounted. At most
`SFB_SECURITY_MAX_CERTS` (8) per class load; unparsable files are skipped.

## Class A: fastboot challenge-response authorization

With class A certificates present the device is **locked**. In locked mode:

- The Advanced menu shows only `Enter Fastboot` - the authorization
  interface.
- fastboot rejects `flash:`, `erase:`, `boot` and partition `fetch:` with
  `FAIL sb-locked...`. Only `getvar:`, `download:`, `fetch:sb_challenge`,
  `flash:sb_challenge.sig`, `reboot` and `exit` are available.

Unlock flow (SSH-style; one attempt per nonce, success or failure):

```
fastboot fetch sb_challenge challenge.bin      # 32-byte nonce
openssl dgst -sha256 -sign auth.key challenge.bin > sb_challenge.sig
fastboot flash sb_challenge.sig sb_challenge.sig
```

The signature is a raw RSA/ECDSA signature (PKCS#1 v1.5 over SHA-256 of the
nonce, as `openssl dgst -sign` produces). It is verified against every
AUTH_CERTS key; a match grants **temporary privilege for the rest of the
boot**: full fastboot command set and full Advanced menu. Signature
verification is unaffected - BOOT_CERTS enforcement always runs. A reboot
re-locks. The nonce is generated from the
EFI RNG protocol when available, otherwise from timer/performance-counter
mixing; it is zeroed after every attempt, so a captured challenge cannot be
replayed and the same nonce cannot be signed twice.

Helper: `tools/sfbsec/fastboot_unlock.sh <auth.key>` runs the whole flow.

## Class B: EFI image verification

With class B certificates present, every image BDS launches is verified
through the EFI Security2 arch protocol (`SfbAllowAuth` in
`SuperFbEntries.c` -> `SfbSecurityVerifyImage`):

1. Parse the PE, locate the certificate table (security data directory).
2. Compute the **authentihash**: SHA-256 over the whole file except the COFF
   checksum field, the security data-directory entry and the certificate
   table itself.
3. Verify the embedded PKCS#7 (WIN_CERTIFICATE, type PKCS_SIGNED_DATA)
   signature over that hash against every BOOT_CERTS key.

An unsigned image is refused; a tampered one is refused. Privilege never
exempts an image: verification runs with or without it.

Signing tool: `tools/sfbsec/sign_efi.py <boot.cer> <boot.key> <app.efi>` -
appends the PKCS#7 (openssl `smime -sign -noattr -binary`) as the
certificate table entry. Note this is the SFB profile (signature directly
over the authentihash stream); it is verified by the host round-trip test
(`tests/sfb/security_roundtrip.py`) against the firmware implementation, not
the Windows Authenticode profile.

## Generating keys and installing certificates

```
tools/sfbsec/gen_certs.sh outdir/
# outdir/auth.cer -> AUTH_CERTS/   (locks the device)
# outdir/boot.cer -> BOOT_CERTS/   (enables verification)
```

Copy the `.cer` files into the container - via the toolkit persist folder
(like `ENTRIES.CFG`), USB export of the config store, or by editing the
container image directly. Keep the `.key` files private; the device never
needs them. Removing all `.cer` files from a directory switches that feature
off again on the next boot.

## Tests

- `tests/sfb/security_roundtrip.py` (also run by `tests/sfb/test_sfb.py`):
  compiles the real `SfbSecurityLib` + mbedtls sources on the host and
  round-trips both classes against openssl-generated key material -
  forged/stale/replayed challenges rejected, tampered and unsigned images
  rejected, privileged boot still verifies.
- `tests/sfb/qemu_security_test.py` (headless QEMU, S1-S6 in
  `MANUAL_TESTS.md`): empty cert dirs behave exactly as before; BOOT_CERTS
  rejects unsigned EFI apps and accepts `sign_efi.py`-signed ones;
  AUTH_CERTS locks the Advanced menu; with both classes verification still
  runs independently at boot; with only BOOT_CERTS the menu is privileged
  with full rows. Fastboot USB cannot run under QEMU (the fastboot stack
  needs the Qualcomm USB device controller), so the challenge flow stays
  a real-device test (R1-R5).
- There are deliberately no test hooks in the production image; the
  certificate-driven switches are the test seams.
